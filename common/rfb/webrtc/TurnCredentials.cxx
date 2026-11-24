/* Copyright (C) 2026 Kasm Technologies Corp.  All Rights Reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#include <rfb/webrtc/TurnCredentials.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <rfb/LogWriter.h>
#include <rfb/ServerCore.h>

namespace rfb::turn {

    namespace {
        LogWriter vlog("TurnCredentials");

        using Clock = std::chrono::steady_clock;

        constexpr size_t MAX_SECRET_BYTES = 4096;
        constexpr size_t MAX_BODY_BYTES = 16384;
        constexpr size_t MAX_RESPONSE_BYTES = 64 * 1024;
        constexpr auto REQUEST_DEADLINE = std::chrono::seconds(5);
        constexpr auto RETRY_AFTER_FAILURE = std::chrono::seconds(30);
        constexpr std::time_t EXPIRY_MARGIN_SECONDS = 2;

        struct FetchError {
            std::string reason;
        };

        class Socket {
        public:
            explicit Socket(int fd_) : fd(fd_) {}
            ~Socket() {
                if (fd >= 0)
                    ::close(fd);
            }
            Socket(const Socket &) = delete;
            Socket &operator=(const Socket &) = delete;
            int get() const {
                return fd;
            }

        private:
            int fd;
        };

        struct SslCtxDeleter {
            void operator()(SSL_CTX *ctx) const {
                SSL_CTX_free(ctx);
            }
        };
        struct SslDeleter {
            void operator()(SSL *ssl) const {
                SSL_free(ssl);
            }
        };

        bool waitFor(int fd, short events, Clock::time_point deadline) {
            for (;;) {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
                if (remaining <= 0)
                    return false;
                pollfd p{fd, events, 0};
                const int ready = ::poll(&p, 1, static_cast<int>(std::min<long long>(remaining, 1000)));
                if (ready < 0 && errno == EINTR)
                    continue;
                if (ready < 0)
                    return false;
                if (ready > 0)
                    return true;
            }
        }

        // getaddrinfo cannot be bounded; this runs only on the refresh thread.
        int connectTcp(const HttpsEndpoint &endpoint, Clock::time_point deadline) {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo *found = nullptr;
            if (getaddrinfo(endpoint.host.c_str(), std::to_string(endpoint.port).c_str(), &hints, &found) != 0)
                throw FetchError{"dns_failure"};
            std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(found, freeaddrinfo);
            bool timedOut = false;
            for (const addrinfo *a = addresses.get(); a; a = a->ai_next) {
                const int fd = ::socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
                if (fd < 0)
                    continue;
                if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0)
                    return fd;
                if (errno == EINPROGRESS) {
                    if (waitFor(fd, POLLOUT, deadline)) {
                        int error = 0;
                        socklen_t len = sizeof(error);
                        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0)
                            return fd;
                    } else {
                        timedOut = Clock::now() >= deadline;
                    }
                }
                ::close(fd);
                if (timedOut)
                    break;
            }
            throw FetchError{timedOut ? "timeout" : "connection_failure"};
        }

        // Drives a non-blocking SSL call until it completes or the deadline passes.
        template<typename Operation>
        int sslRetry(SSL *ssl, int fd, Clock::time_point deadline, Operation operation) {
            for (;;) {
                ERR_clear_error();
                const int result = operation();
                if (result > 0)
                    return result;
                const int error = SSL_get_error(ssl, result);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                    if (!waitFor(fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline))
                        throw FetchError{"timeout"};
                    continue;
                }
                return -error;
            }
        }

        std::string exchange(const HttpsEndpoint &endpoint, const std::string &request, const std::string &caFile,
            bool skipVerify, Clock::time_point deadline) {
            std::unique_ptr<SSL_CTX, SslCtxDeleter> ctx(SSL_CTX_new(TLS_client_method()));
            if (!ctx || !SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION))
                throw FetchError{"tls_configuration"};
            // Many servers close after Connection: close without close_notify; HTTP framing still detects truncation.
            SSL_CTX_set_options(ctx.get(), SSL_OP_IGNORE_UNEXPECTED_EOF);
            if (skipVerify) {
                SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr);
            } else {
                const bool loaded = caFile.empty() ? SSL_CTX_set_default_verify_paths(ctx.get()) == 1
                                                   : SSL_CTX_load_verify_locations(ctx.get(), caFile.c_str(), nullptr) == 1;
                if (!loaded)
                    throw FetchError{"tls_configuration"};
                SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
            }

            Socket socket(connectTcp(endpoint, deadline));
            std::unique_ptr<SSL, SslDeleter> ssl(SSL_new(ctx.get()));
            if (!ssl || SSL_set_fd(ssl.get(), socket.get()) != 1)
                throw FetchError{"tls_configuration"};
            if (!endpoint.ipLiteral && SSL_set_tlsext_host_name(ssl.get(), endpoint.host.c_str()) != 1)
                throw FetchError{"tls_configuration"};
            if (!skipVerify) {
                const bool bound = endpoint.ipLiteral
                                       ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl.get()), endpoint.host.c_str()) == 1
                                       : SSL_set1_host(ssl.get(), endpoint.host.c_str()) == 1;
                if (!bound)
                    throw FetchError{"tls_configuration"};
            }

            if (sslRetry(ssl.get(), socket.get(), deadline, [&] { return SSL_connect(ssl.get()); }) <= 0)
                throw FetchError{SSL_get_verify_result(ssl.get()) != X509_V_OK ? "tls_verification" : "tls_handshake"};

            size_t sent = 0;
            while (sent < request.size()) {
                const int n = sslRetry(ssl.get(), socket.get(), deadline, [&] {
                    return SSL_write(ssl.get(), request.data() + sent, static_cast<int>(request.size() - sent));
                });
                if (n <= 0)
                    throw FetchError{"connection_failure"};
                sent += static_cast<size_t>(n);
            }

            std::string raw;
            char buffer[4096];
            for (;;) {
                const int n = sslRetry(ssl.get(), socket.get(), deadline,
                    [&] { return SSL_read(ssl.get(), buffer, sizeof(buffer)); });
                if (n > 0) {
                    raw.append(buffer, static_cast<size_t>(n));
                    if (raw.size() > MAX_RESPONSE_BYTES)
                        throw FetchError{"response_too_large"};
                    continue;
                }
                if (-n == SSL_ERROR_ZERO_RETURN || -n == SSL_ERROR_SYSCALL)
                    break;
                throw FetchError{"invalid_http_response"};
            }
            return raw;
        }

        std::string contentType(const HttpResponse &response) {
            const auto it = response.headers.find("content-type");
            if (it == response.headers.end())
                return {};
            std::string type = it->second.substr(0, it->second.find(';'));
            type.erase(type.find_last_not_of(" \t") + 1);
            return type;
        }

        std::time_t remainingSeconds(std::time_t expiresAt) {
            return expiresAt - std::time(nullptr);
        }
    } // namespace

    std::optional<std::string> readSecretFile(const std::string &path, std::string &error) {
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto status = fs::status(path, ec);
        if (ec || !fs::is_regular_file(status)) {
            error = "must name a regular file";
            return std::nullopt;
        }
        const auto insecure = fs::perms::group_all | fs::perms::others_all;
        if ((status.permissions() & insecure) != fs::perms::none) {
            error = "must not be accessible by group or others";
            return std::nullopt;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "cannot be read";
            return std::nullopt;
        }
        std::string secret((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r'))
            secret.pop_back();
        if (secret.empty()) {
            error = "is empty";
            return std::nullopt;
        }
        if (secret.size() > MAX_SECRET_BYTES) {
            error = "exceeds 4096 bytes";
            return std::nullopt;
        }
        return secret;
    }

    std::optional<Credentials> fetchCredentials(
        const std::string &url, const std::string &caFile, bool skipVerify, std::string &failureReason) {
        try {
            const auto endpoint = parseHttpsEndpoint(url);
            if (!endpoint)
                throw FetchError{"invalid_endpoint"};
            const char *token = std::getenv("KASM_API_JWT");
            if (!token || !*token)
                throw FetchError{"missing_jwt"};
            if (!wellFormedJwt(token))
                throw FetchError{"malformed_jwt"};

            const bool defaultPort = endpoint->port == 443;
            const std::string hostHeader = (endpoint->host.find(':') != std::string::npos ? "[" + endpoint->host + "]"
                                                                                           : endpoint->host) +
                                           (defaultPort ? "" : ":" + std::to_string(endpoint->port));
            const std::string request = "POST " + endpoint->target + " HTTP/1.1\r\n" + "Host: " + hostHeader +
                                        "\r\n" + "Authorization: Bearer " + token + "\r\n" +
                                        "Accept: application/json\r\n" + "Content-Length: 0\r\n" +
                                        "Connection: close\r\n\r\n";

            const auto raw = exchange(*endpoint, request, caFile, skipVerify, Clock::now() + REQUEST_DEADLINE);
            const auto response = parseHttpResponse(raw, MAX_BODY_BYTES);
            if (!response)
                throw FetchError{"invalid_http_response"};
            const auto requestId = response->headers.count("x-request-id")
                                       ? safeRequestId(response->headers.at("x-request-id"))
                                       : std::string{};
            const auto withId = [&](std::string reason) {
                return requestId.empty() ? reason : reason + " request_id=" + requestId;
            };
            if (response->status != 200)
                throw FetchError{withId("http_error http_status=" + std::to_string(response->status))};
            if (contentType(*response) != "application/json")
                throw FetchError{withId("invalid_content_type")};
            auto creds = parseCredentialResponse(response->body, std::time(nullptr));
            if (!creds)
                throw FetchError{withId("invalid_response")};
            return creds;
        } catch (const FetchError &e) {
            failureReason = e.reason;
        } catch (const std::exception &) {
            failureReason = "internal_error";
        }
        return std::nullopt;
    }

    CredentialSource &CredentialSource::instance() {
        static auto *source = new CredentialSource();
        return *source;
    }

    bool CredentialSource::replaceCredentials(Credentials creds) {
        if (!creds.usable() && !current.credentials.usable())
            return false;
        current.credentials = std::move(creds);
        ++current.generation;
        return true;
    }

    void CredentialSource::start() {
        std::lock_guard<std::mutex> lock(mutex);
        if (started)
            return;
        started = true;
        current.forceRelay = Server::forceTurn;
        const auto staticServers = splitCommaSeparated(static_cast<const char *>(Server::turnServer));

        url = static_cast<const char *>(Server::turnCredentialUrl);
        if (!url.empty()) {
            caFile = static_cast<const char *>(Server::turnCredentialCaFile);
            skipVerify = Server::turnCredentialSkipVerify;
            if (skipVerify)
                vlog.error("WARNING: TURN credential API TLS certificate verification is disabled");
            if (!staticServers.empty())
                vlog.info("TurnCredentialUrl is set; static TurnServer settings are not used");
            std::thread([this] { refreshLoop(); }).detach();
            return;
        }

        if (staticServers.empty()) {
            if (current.forceRelay)
                vlog.error("ForceTurn requires TurnServer or TurnCredentialUrl; relay-only is disabled");
            return;
        }
        Credentials creds;
        for (const auto &server: staticServers)
            creds.urls.push_back(turnServerUrl(server));
        creds.username = static_cast<const char *>(Server::turnUsername);
        const std::string file = static_cast<const char *>(Server::turnCredentialFile);
        if (!file.empty()) {
            std::string error;
            if (auto secret = readSecretFile(file, error))
                creds.credential = std::move(*secret);
            else
                vlog.error("TurnCredentialFile %s; TURN is disabled", error.c_str());
        }
        if (!creds.usable()) {
            vlog.error("TurnServer requires TurnUsername and TurnCredentialFile; TURN is disabled");
            return;
        }
        vlog.info("TURN relays: %s", describeForLog(creds).c_str());
        replaceCredentials(std::move(creds));
    }

    Snapshot CredentialSource::snapshot() {
        start();
        std::lock_guard<std::mutex> lock(mutex);
        return current;
    }

    void CredentialSource::refreshLoop() {
        for (;;) {
            std::string reason;
            auto fetched = fetchCredentials(url, caFile, skipVerify, reason);

            std::unique_lock<std::mutex> lock(mutex);
            auto next = Clock::now() + RETRY_AFTER_FAILURE;
            if (fetched) {
                const auto remaining = remainingSeconds(fetched->expiresAt);
                next = Clock::now() + std::chrono::seconds(std::max<std::time_t>(1, remaining / 2));
                vlog.info("TURN credentials refreshed: %s (expire in %lds)", describeForLog(*fetched).c_str(),
                    static_cast<long>(remaining));
                replaceCredentials(std::move(*fetched));
            } else if (current.credentials.usable()) {
                vlog.error("TURN credential refresh failed (%s); retaining current unexpired credentials (retry in 30s)",
                    reason.c_str());
            } else {
                vlog.error("TURN credential retrieval failed (%s); using direct/STUN connectivity (retry in 30s)",
                    reason.c_str());
            }

            while (Clock::now() < next) {
                auto wake = next;
                if (current.credentials.usable()) {
                    const auto untilExpiry = remainingSeconds(current.credentials.expiresAt) - EXPIRY_MARGIN_SECONDS;
                    if (untilExpiry <= 0) {
                        replaceCredentials({});
                        vlog.error("TURN credentials expired; using direct/STUN connectivity");
                        continue;
                    }
                    wake = std::min(wake, Clock::now() + std::chrono::seconds(untilExpiry));
                }
                lock.unlock();
                std::this_thread::sleep_until(wake);
                lock.lock();
            }
        }
    }

} // namespace rfb::turn

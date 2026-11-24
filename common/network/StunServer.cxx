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

#include <network/StunServer.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <random>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace network {

    namespace {
        constexpr const char *stunScheme = "stun:";
        constexpr const char *unsupportedSchemes[] = {"stuns:", "turn:", "turns:"};

        bool startsWith(const std::string &s, const char *prefix) {
            return s.rfind(prefix, 0) == 0;
        }

        bool hostnameChar(unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '-' || c == '_';
        }

        std::optional<uint16_t> parsePort(const std::string &text) {
            unsigned value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || text.front() == '0' || error != std::errc{} || end != text.data() + text.size() ||
                value > 65535)
                return std::nullopt;
            return static_cast<uint16_t>(value);
        }
    } // namespace

    std::string StunServer::iceUrl() const {
        const bool ipv6 = host.find(':') != std::string::npos;
        return std::string(stunScheme) + (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    }

    bool usesDefaultStunServers(const std::string &setting) {
        return setting.empty() || setting == "auto";
    }

    std::optional<StunServer> parseStunServer(const std::string &setting) {
        for (const char *scheme: unsupportedSchemes)
            if (startsWith(setting, scheme))
                return std::nullopt;

        const std::string authority = startsWith(setting, stunScheme) ? setting.substr(strlen(stunScheme)) : setting;
        StunServer server;
        std::optional<std::string> portText;
        if (startsWith(authority, "[")) {
            const auto close = authority.find(']');
            if (close == std::string::npos)
                return std::nullopt;
            server.host = authority.substr(1, close - 1);
            in6_addr literal;
            if (inet_pton(AF_INET6, server.host.c_str(), &literal) != 1)
                return std::nullopt;
            const std::string rest = authority.substr(close + 1);
            if (!rest.empty()) {
                if (rest.front() != ':')
                    return std::nullopt;
                portText = rest.substr(1);
            }
        } else {
            const auto colon = authority.find(':');
            server.host = authority.substr(0, colon);
            if (server.host.empty() || !std::all_of(server.host.begin(), server.host.end(), hostnameChar))
                return std::nullopt;
            if (colon != std::string::npos)
                portText = authority.substr(colon + 1);
        }

        if (portText) {
            const auto port = parsePort(*portText);
            if (!port)
                return std::nullopt;
            server.port = *port;
        }
        return server;
    }

    const std::vector<StunServer> &defaultStunServers() {
        static const std::vector<StunServer> servers = {
            {"stun.l.google.com", 19302},  {"stun1.l.google.com", 19302}, {"stun2.l.google.com", 19302},
            {"stun3.l.google.com", 19302}, {"stun4.l.google.com", 19302}, {"stun.voipbuster.com"},
            {"stun.voipstunt.com"},
        };
        return servers;
    }

    std::vector<std::string> defaultIceStunUrls(const size_t firstReachable) {
        const auto &servers = defaultStunServers();
        const size_t first = std::min(firstReachable, servers.size() - 1);
        std::vector<std::string> urls;
        for (size_t i = first; i < std::min(first + maxDefaultIceServers, servers.size()); i++)
            urls.push_back(servers[i].iceUrl());
        return urls;
    }

    namespace {
        // Hostnames prefer IPv4 so dual-stack servers keep yielding an IPv4 public address.
        std::optional<sockaddr_storage> resolve(const StunServer &server, socklen_t &length) {
            const bool literal6 = server.host.find(':') != std::string::npos;
            const std::vector<int> families = literal6 ? std::vector<int>{AF_INET6} : std::vector<int>{AF_INET, AF_INET6};
            for (const int family: families) {
                addrinfo hints{};
                hints.ai_family = family;
                hints.ai_socktype = SOCK_DGRAM;
                hints.ai_flags = literal6 ? AI_NUMERICHOST : 0;
                addrinfo *result = nullptr;
                const std::string port = std::to_string(server.port);
                if (getaddrinfo(server.host.c_str(), port.c_str(), &hints, &result) != 0 || !result)
                    continue;
                sockaddr_storage address{};
                memcpy(&address, result->ai_addr, result->ai_addrlen);
                length = result->ai_addrlen;
                freeaddrinfo(result);
                return address;
            }
            return std::nullopt;
        }

        bool sameEndpoint(const sockaddr_storage &a, const sockaddr_storage &b) {
            if (a.ss_family != b.ss_family)
                return false;
            if (a.ss_family == AF_INET) {
                const auto &x = reinterpret_cast<const sockaddr_in &>(a), &y = reinterpret_cast<const sockaddr_in &>(b);
                return x.sin_port == y.sin_port && x.sin_addr.s_addr == y.sin_addr.s_addr;
            }
            const auto &x = reinterpret_cast<const sockaddr_in6 &>(a), &y = reinterpret_cast<const sockaddr_in6 &>(b);
            return x.sin6_port == y.sin6_port && !memcmp(&x.sin6_addr, &y.sin6_addr, sizeof(in6_addr));
        }

        // MAPPED-ADDRESS value: reserved(1) family(1) port(2) address(4 or 16).
        std::optional<std::string> mappedAddress(const unsigned char *buf, const size_t len) {
            constexpr size_t headerSize = 20;
            for (size_t i = headerSize; i + 4 <= len;) {
                const uint16_t type = (buf[i] << 8) | buf[i + 1];
                const uint16_t attrLen = (buf[i + 2] << 8) | buf[i + 3];
                i += 4;
                if (i + attrLen > len)
                    return std::nullopt;
                if (type == 0x0001 && attrLen >= 4) {
                    const int family = buf[i + 1] == 0x01 ? AF_INET : buf[i + 1] == 0x02 ? AF_INET6 : AF_UNSPEC;
                    const size_t addrLen = family == AF_INET ? 4 : family == AF_INET6 ? 16 : 0;
                    char text[INET6_ADDRSTRLEN];
                    if (!addrLen || attrLen < 4 + addrLen || !inet_ntop(family, &buf[i + 4], text, sizeof(text)))
                        return std::nullopt;
                    return std::string(text);
                }
                i += attrLen;
            }
            return std::nullopt;
        }

        class UdpSocket {
          public:
            explicit UdpSocket(const int family) : fd(socket(family, SOCK_DGRAM, 0)) {}
            ~UdpSocket() {
                if (fd >= 0)
                    close(fd);
            }
            UdpSocket(const UdpSocket &) = delete;
            UdpSocket &operator=(const UdpSocket &) = delete;
            const int fd;
        };
    } // namespace

    std::optional<std::string> queryStunMappedAddress(const StunServer &server, const int timeoutMs) {
        socklen_t dstLen = 0;
        const auto dst = resolve(server, dstLen);
        if (!dst)
            return std::nullopt;
        const UdpSocket sock(dst->ss_family);
        if (sock.fd < 0)
            return std::nullopt;

        constexpr size_t headerSize = 20;
        unsigned char request[headerSize] = {0x00, 0x01}; // binding request, zero-length body
        std::random_device random;
        std::generate(request + 4, request + headerSize, [&] { return static_cast<unsigned char>(random()); });
        if (sendto(sock.fd, request, headerSize, 0, reinterpret_cast<const sockaddr *>(&*dst), dstLen) !=
            static_cast<ssize_t>(headerSize))
            return std::nullopt;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (true) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            pollfd pfd{sock.fd, POLLIN, 0};
            if (remaining.count() <= 0 || poll(&pfd, 1, static_cast<int>(remaining.count())) <= 0)
                return std::nullopt;

            unsigned char buf[1500];
            sockaddr_storage from{};
            socklen_t fromLen = sizeof(from);
            const ssize_t len = recvfrom(sock.fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
            // Stray datagrams are skipped so they cannot cut the wait short or answer for the server.
            if (len < static_cast<ssize_t>(headerSize) || !sameEndpoint(from, *dst) ||
                memcmp(buf + 4, request + 4, headerSize - 4))
                continue;
            if (buf[0] != 0x01 || buf[1] != 0x01)
                return std::nullopt; // not a binding success response
            return mappedAddress(buf, static_cast<size_t>(len));
        }
    }

} // namespace network

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

#include <rfb/webrtc/TurnConfig.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <regex>

#include <network/cJSON.h>

namespace rfb::turn {

    namespace {
        bool startsWith(const std::string &s, const char *prefix) {
            return s.rfind(prefix, 0) == 0;
        }

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        std::string trim(const std::string &s) {
            const auto first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return {};
            const auto last = s.find_last_not_of(" \t\r\n");
            return s.substr(first, last - first + 1);
        }

        bool parsePort(const std::string &text, uint16_t &port) {
            unsigned value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value == 0 ||
                value > 65535)
                return false;
            port = static_cast<uint16_t>(value);
            return true;
        }

        bool splitHostPort(const std::string &authority, std::string &host, std::optional<uint16_t> &port) {
            if (authority.empty() || authority.find('@') != std::string::npos)
                return false;
            std::string rest;
            if (authority.front() == '[') {
                const auto close = authority.find(']');
                if (close == std::string::npos || close == 1)
                    return false;
                host = authority.substr(1, close - 1);
                rest = authority.substr(close + 1);
            } else {
                const auto colon = authority.find(':');
                host = authority.substr(0, colon);
                rest = colon == std::string::npos ? std::string{} : authority.substr(colon);
            }
            if (host.empty())
                return false;
            if (rest.empty())
                return true;
            uint16_t parsed = 0;
            if (rest.front() != ':' || !parsePort(rest.substr(1), parsed))
                return false;
            port = parsed;
            return true;
        }

        bool printableToken(const std::string &value) {
            return !value.empty() && value.size() <= 1024 &&
                   std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
        }
    } // namespace

    std::vector<std::string> splitCommaSeparated(const std::string &value) {
        std::vector<std::string> values;
        size_t offset = 0;
        while (offset <= value.size()) {
            const size_t comma = value.find(',', offset);
            auto item = trim(value.substr(offset, comma == std::string::npos ? std::string::npos : comma - offset));
            if (!item.empty())
                values.push_back(std::move(item));
            if (comma == std::string::npos)
                break;
            offset = comma + 1;
        }
        return values;
    }

    std::string turnServerUrl(const std::string &server) {
        if (startsWith(server, "turn:") || startsWith(server, "turns:"))
            return server;
        return "turn:" + server;
    }

    std::string jsonEscape(const std::string &value) {
        std::string out;
        out.reserve(value.size() + 2);
        for (const unsigned char c: value) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    }

    std::string iceConfigJson(const std::vector<std::string> &stunUrls, const Credentials *turn, bool forceRelay) {
        auto urlArray = [](const std::vector<std::string> &urls) {
            std::string json = "[";
            for (size_t i = 0; i < urls.size(); ++i) {
                if (i)
                    json += ",";
                json += "\"" + jsonEscape(urls[i]) + "\"";
            }
            return json + "]";
        };
        std::string servers = "[";
        bool added = false;
        if (!stunUrls.empty()) {
            servers += "{\"urls\":" + urlArray(stunUrls) + "}";
            added = true;
        }
        const bool haveTurn = turn && turn->usable();
        if (haveTurn) {
            if (added)
                servers += ",";
            servers += "{\"urls\":" + urlArray(turn->urls) + ",\"username\":\"" + jsonEscape(turn->username) +
                       "\",\"credential\":\"" + jsonEscape(turn->credential) + "\"}";
        }
        servers += "]";
        // Relay-only with no relay leaves the browser no candidates, so the policy follows usable credentials.
        const bool relay = forceRelay && haveTurn;
        return "{\"iceServers\":" + servers + ",\"iceTransportPolicy\":\"" + (relay ? "relay" : "all") + "\"}";
    }

    std::optional<TurnUrl> parseTurnUrl(const std::string &url) {
        bool secure;
        std::string rest;
        if (startsWith(url, "turns:")) {
            secure = true;
            rest = url.substr(6);
        } else if (startsWith(url, "turn:")) {
            secure = false;
            rest = url.substr(5);
        } else {
            return std::nullopt;
        }
        std::string query;
        if (const auto q = rest.find('?'); q != std::string::npos) {
            query = rest.substr(q + 1);
            rest.resize(q);
        }
        TurnUrl parsed;
        std::optional<uint16_t> port;
        if (!splitHostPort(rest, parsed.host, port))
            return std::nullopt;
        parsed.port = port.value_or(secure ? 5349 : 3478);
        std::string transport = secure ? "tcp" : "udp";
        if (!query.empty()) {
            if (!startsWith(query, "transport=") || query.find('&') != std::string::npos)
                return std::nullopt;
            transport = lower(query.substr(10));
        }
        if (transport == "udp" && !secure)
            parsed.transport = TurnUrl::Transport::Udp;
        else if (transport == "tcp")
            parsed.transport = secure ? TurnUrl::Transport::Tls : TurnUrl::Transport::Tcp;
        else
            return std::nullopt;
        return parsed;
    }

    std::optional<Credentials> parseCredentialResponse(const std::string &json, std::time_t now) {
        cJSON *root = cJSON_Parse(json.c_str());
        if (!root)
            return std::nullopt;
        std::optional<Credentials> result;
        do {
            if (!cJSON_IsObject(root))
                break;
            const cJSON *user = cJSON_GetObjectItemCaseSensitive(root, "username");
            const cJSON *credential = cJSON_GetObjectItemCaseSensitive(root, "credential");
            const cJSON *urls = cJSON_GetObjectItemCaseSensitive(root, "urls");
            const cJSON *expiry = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
            if (!cJSON_IsString(user) || !cJSON_IsString(credential) || !printableToken(user->valuestring) ||
                !printableToken(credential->valuestring))
                break;
            if (!cJSON_IsArray(urls))
                break;
            const int count = cJSON_GetArraySize(urls);
            if (count < 1 || count > 8)
                break;
            static const std::regex turnUrl(R"(^turns?:[^\s,]+$)");
            Credentials creds;
            bool urlsValid = true;
            const cJSON *url;
            cJSON_ArrayForEach(url, urls) {
                if (!cJSON_IsString(url) || std::string(url->valuestring).size() > 1024 ||
                    !std::regex_match(url->valuestring, turnUrl)) {
                    urlsValid = false;
                    break;
                }
                creds.urls.emplace_back(url->valuestring);
            }
            if (!urlsValid)
                break;
            if (!cJSON_IsNumber(expiry) || std::floor(expiry->valuedouble) != expiry->valuedouble)
                break;
            const double expires = expiry->valuedouble;
            if (!(expires > static_cast<double>(now) + 5 && expires <= static_cast<double>(now) + 86400))
                break;
            creds.username = user->valuestring;
            creds.credential = credential->valuestring;
            creds.expiresAt = static_cast<std::time_t>(expires);
            result = std::move(creds);
        } while (false);
        cJSON_Delete(root);
        return result;
    }

    std::optional<HttpsEndpoint> parseHttpsEndpoint(const std::string &url) {
        if (!startsWith(lower(url.substr(0, 8)), "https://"))
            return std::nullopt;
        if (url.find('#') != std::string::npos ||
            std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
            return std::nullopt;
        const std::string rest = url.substr(8);
        const auto slash = rest.find_first_of("/?");
        const std::string authority = rest.substr(0, slash);
        HttpsEndpoint endpoint;
        std::optional<uint16_t> port;
        if (!splitHostPort(authority, endpoint.host, port))
            return std::nullopt;
        endpoint.port = port.value_or(443);
        endpoint.ipLiteral = authority.front() == '[' ||
                             std::all_of(endpoint.host.begin(), endpoint.host.end(),
                                 [](unsigned char c) { return std::isdigit(c) || c == '.'; });
        endpoint.target = slash == std::string::npos ? "/" : rest.substr(slash);
        if (endpoint.target.front() == '?')
            endpoint.target = "/" + endpoint.target;
        return endpoint;
    }

    std::optional<HttpResponse> parseHttpResponse(const std::string &raw, size_t maxBody) {
        const auto headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos)
            return std::nullopt;
        HttpResponse response;
        size_t lineStart = 0;
        bool first = true;
        while (lineStart < headerEnd) {
            auto lineEnd = raw.find("\r\n", lineStart);
            if (lineEnd == std::string::npos || lineEnd > headerEnd)
                lineEnd = headerEnd;
            const std::string line = raw.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 2;
            if (first) {
                first = false;
                static const std::regex statusLine(R"(^HTTP/1\.[01] ([0-9]{3})(?: .*)?$)");
                std::smatch m;
                if (!std::regex_match(line, m, statusLine))
                    return std::nullopt;
                response.status = std::stoi(m[1].str());
                continue;
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos || colon == 0)
                return std::nullopt;
            response.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }
        if (first)
            return std::nullopt;
        std::string payload = raw.substr(headerEnd + 4);
        const auto te = response.headers.find("transfer-encoding");
        if (te != response.headers.end() && lower(te->second) == "chunked") {
            size_t pos = 0;
            for (;;) {
                const auto sizeEnd = payload.find("\r\n", pos);
                if (sizeEnd == std::string::npos)
                    return std::nullopt;
                std::string sizeText = payload.substr(pos, sizeEnd - pos);
                if (const auto ext = sizeText.find(';'); ext != std::string::npos)
                    sizeText.resize(ext);
                sizeText = trim(sizeText);
                size_t chunk = 0;
                const auto [end, error] =
                    std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), chunk, 16);
                if (sizeText.empty() || error != std::errc{} || end != sizeText.data() + sizeText.size())
                    return std::nullopt;
                pos = sizeEnd + 2;
                if (chunk == 0)
                    break;
                if (chunk > maxBody || response.body.size() + chunk > maxBody ||
                    payload.size() < pos + chunk + 2 || payload.compare(pos + chunk, 2, "\r\n") != 0)
                    return std::nullopt;
                response.body.append(payload, pos, chunk);
                pos += chunk + 2;
            }
            return response;
        }
        const auto cl = response.headers.find("content-length");
        if (cl != response.headers.end()) {
            size_t length = 0;
            const auto &text = cl->second;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), length);
            if (text.empty() || error != std::errc{} || end != text.data() + text.size() || length > maxBody ||
                payload.size() < length)
                return std::nullopt;
            payload.resize(length);
        }
        if (payload.size() > maxBody)
            return std::nullopt;
        response.body = std::move(payload);
        return response;
    }

    std::string safeRequestId(const std::string &value) {
        static const std::regex id(
            R"(^(?:[a-fA-F0-9]{32}|[a-fA-F0-9]{8}(?:-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12})$)");
        return std::regex_match(value, id) ? value : std::string{};
    }

    bool wellFormedJwt(const std::string &token) {
        static const std::regex jwt(R"(^[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$)");
        return !token.empty() && token.size() <= 16384 && std::regex_match(token, jwt);
    }

    std::string describeForLog(const Credentials &creds) {
        std::string out;
        for (const auto &url: creds.urls) {
            if (!out.empty())
                out += ", ";
            const auto parsed = parseTurnUrl(url);
            if (!parsed) {
                out += "<unparsed>";
                continue;
            }
            const char *transport = parsed->transport == TurnUrl::Transport::Udp   ? "udp"
                                    : parsed->transport == TurnUrl::Transport::Tcp ? "tcp"
                                                                                   : "tls";
            out += parsed->host + ":" + std::to_string(parsed->port) + "/" + transport;
        }
        return out;
    }

} // namespace rfb::turn

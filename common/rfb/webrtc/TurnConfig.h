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

// Interprets network.udp.turn_* exactly as the chrome-cdp backend does, so the
// same kasmvnc.yaml behaves identically under either backend.

#pragma once

#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rfb::turn {

    struct Credentials {
        std::vector<std::string> urls;
        std::string username;
        std::string credential;
        std::time_t expiresAt = 0;

        bool usable() const {
            return !urls.empty() && !username.empty() && !credential.empty();
        }
    };

    std::vector<std::string> splitCommaSeparated(const std::string &value);
    std::string turnServerUrl(const std::string &server);
    std::string jsonEscape(const std::string &value);

    // Shaped as an RTCConfiguration subset so the browser can pass it to RTCPeerConnection unchanged.
    std::string iceConfigJson(const std::vector<std::string> &stunUrls, const Credentials *turn, bool forceRelay);

    struct TurnUrl {
        enum class Transport { Udp, Tcp, Tls };
        std::string host;
        uint16_t port = 3478;
        Transport transport = Transport::Udp;
    };
    std::optional<TurnUrl> parseTurnUrl(const std::string &url);

    std::optional<Credentials> parseCredentialResponse(const std::string &json, std::time_t now);

    struct HttpsEndpoint {
        std::string host;
        uint16_t port = 443;
        std::string target;
        bool ipLiteral = false;
    };
    std::optional<HttpsEndpoint> parseHttpsEndpoint(const std::string &url);

    struct HttpResponse {
        int status = 0;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    std::optional<HttpResponse> parseHttpResponse(const std::string &raw, size_t maxBody);

    std::string safeRequestId(const std::string &value);
    bool wellFormedJwt(const std::string &token);
    std::string describeForLog(const Credentials &creds);

} // namespace rfb::turn

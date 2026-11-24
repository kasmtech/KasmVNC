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

// The StunServer setting feeds both public-IP discovery and WebRTC ICE, so
// both read it through this one grammar: hostname[:port] or stun:hostname[:port].

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace network {

    struct StunServer {
        static constexpr uint16_t defaultPort = 3478;

        std::string host;
        uint16_t port = defaultPort;

        std::string iceUrl() const;
    };

    // Empty and "auto" select the built-in server list.
    bool usesDefaultStunServers(const std::string &setting);

    // Rejects stuns:, turn:, turns: and query parameters, which discovery cannot speak.
    std::optional<StunServer> parseStunServer(const std::string &setting);

    // Built-in servers, in the order public-IP discovery tries them.
    const std::vector<StunServer> &defaultStunServers();

    // Browsers query every ICE server they are given and warn when there are many.
    constexpr size_t maxDefaultIceServers = 2;

    // Every server before firstReachable failed discovery, so ICE starts at the one that answered.
    std::vector<std::string> defaultIceStunUrls(size_t firstReachable);

    // Classic (RFC 3489) binding request over IPv4 or IPv6; returns the MAPPED-ADDRESS as text.
    std::optional<std::string> queryStunMappedAddress(const StunServer &server, int timeoutMs);

} // namespace network

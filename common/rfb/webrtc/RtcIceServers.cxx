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

#include <rfb/webrtc/RtcIceServers.h>

#include <rfb/LogWriter.h>

namespace rfb::turn {

    namespace {
        LogWriter vlog("RtcIceServers");
    }

    std::vector<rtc::IceServer> rtcIceServers(const std::vector<std::string> &stunUrls, const Credentials &turn) {
        std::vector<rtc::IceServer> servers;
        for (const auto &url: stunUrls) {
            try {
                servers.emplace_back(url);
            } catch (const std::exception &e) {
                vlog.error("STUN server '%s' rejected: %s", url.c_str(), e.what());
            }
        }
        if (!turn.usable())
            return servers;
        bool serverRelay = false;
        for (const auto &url: turn.urls) {
            const auto parsed = parseTurnUrl(url);
            if (!parsed) {
                vlog.error("TURN server URL '%s' rejected", url.c_str());
                continue;
            }
            if (parsed->transport != TurnUrl::Transport::Udp)
                continue;
            servers.emplace_back(parsed->host, parsed->port, turn.username, turn.credential,
                rtc::IceServer::RelayType::TurnUdp);
            serverRelay = true;
        }
        static bool warnedNoServerRelay = false;
        if (!serverRelay && !warnedNoServerRelay) {
            warnedNoServerRelay = true;
            vlog.info("No UDP TURN URL configured: TCP/TLS TURN is used by the browser only; "
                      "the server gathers no relay candidate and must reach the TURN relay directly over UDP");
        }
        return servers;
    }

} // namespace rfb::turn

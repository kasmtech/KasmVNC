/* Copyright (C) 2026 Kasm.  All Rights Reserved.
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

#ifndef __NETWORK_WEBRTC_PUBLIC_IP_H__
#define __NETWORK_WEBRTC_PUBLIC_IP_H__

#include <optional>
#include <stdexcept>
#include <string>

#include <rtc/candidate.hpp>

namespace network {

    inline bool usablePublicIP(const std::string &ip) {
        return !ip.empty() && ip.rfind("127.", 0) != 0 && ip != "::1";
    }

    inline std::optional<rtc::Candidate> publicIPCandidate(const rtc::Candidate &cand, const std::string &publicIP) {
        if (!usablePublicIP(publicIP) || cand.type() != rtc::Candidate::Type::Host)
            return std::nullopt;
        const auto addr = cand.address();
        const auto port = cand.port();
        if (!addr || !port || *addr == publicIP)
            return std::nullopt;
        const bool publicIsV6 = publicIP.find(':') != std::string::npos;
        const bool candIsV6 = cand.family() == rtc::Candidate::Family::Ipv6;
        if (publicIsV6 != candIsV6)
            return std::nullopt;
        try {
            rtc::Candidate mirrored = cand;
            mirrored.changeAddress(publicIP, *port);
            return mirrored;
        } catch (const std::invalid_argument &) {
            return std::nullopt;
        }
    }

} // namespace network

#endif

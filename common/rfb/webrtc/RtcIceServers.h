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

#pragma once

#include <string>
#include <vector>

#include <rtc/configuration.hpp>

#include <rfb/webrtc/TurnConfig.h>

namespace rfb::turn {

    // libjuice relays only over UDP, so TCP/TLS TURN entries serve the browser alone.
    std::vector<rtc::IceServer> rtcIceServers(const std::vector<std::string> &stunUrls, const Credentials &turn);

} // namespace rfb::turn

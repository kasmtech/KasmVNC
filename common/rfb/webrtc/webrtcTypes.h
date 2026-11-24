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
#ifndef KASMVNC_WEBRTCTYPES_H
#define KASMVNC_WEBRTCTYPES_H

#include <cstddef>
#include <cstdint>

#include <rdr/types.h>

namespace rfb {

  // These types ride the msgTypeWebRTCSignal message (rfb/msgTypes.h).
  constexpr uint8_t WEBRTC_SESSION_SCREEN = 0xFF;
  constexpr uint8_t WEBRTC_IMAGE_SCREEN = 0xFE;

  constexpr uint8_t MAX_WEBRTC_SCREENS = 8;

  constexpr std::size_t MAX_WEBRTC_SIGNAL_BYTES = 16 * 1024;

  enum class WebRTCSignalKind : uint8_t
  {
      SdpOffer = 1,
      SdpAnswer = 2,
      IceCandidate = 3,
      IceServers = 4,
      Fallback = 5,
      ClientCapabilities = 6,
      Close = 7,
      RequestOffer = 8,
      ImageRequest = 9,
  };
}

#endif // KASMVNC_WEBRTCTYPES_H

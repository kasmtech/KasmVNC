/* Copyright (C) 2025 Kasm.  All Rights Reserved.
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

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <utility>

namespace rfb {
    // NOTE: SupportedEncoderCount represents the number of actively supported codec families (H264, H265, AV1)
    static constexpr unsigned int SupportedEncoderCount = 5;

    // Compression control
    static constexpr unsigned int kasmVideoH264 = 0x01 << 4; // H.264 encoding
    static constexpr unsigned int kasmVideoH265 = 0x02 << 4; // H.265 encoding
    static constexpr unsigned int kasmVideoAV1 = 0x03 << 4; // AV1 encoding
    static constexpr unsigned int kasmVideoVP8 = 0x04 << 4; // VP8 encoding (not yet supported)
    static constexpr unsigned int kasmVideoVP9 = 0x05 << 4; // VP9 encoding (not yet supported)
    static constexpr unsigned int kasmVideoSkip = 0x00 << 4; // Skip frame

    using KasmVideoCodecId = uint8_t;
    static constexpr KasmVideoCodecId kasmVideoUnknownCodec = 0;

    // Maps a codec token from the client (ClientCapabilities) to its ID. Matching ignores case.
    inline KasmVideoCodecId kasmVideoCodecFromToken(std::string_view token) {
        static constexpr auto table = std::to_array<std::pair<std::string_view, KasmVideoCodecId>>({
            {"h264", kasmVideoH264},
            {"h265", kasmVideoH265},
            {"av1",  kasmVideoAV1 },
        });
        const auto equalIgnoreCase = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        };
        for (const auto &[name, id]: table) {
            if (std::ranges::equal(name, token, equalIgnoreCase))
                return id;
        }
        return kasmVideoUnknownCodec;
    }

    inline const char *kasmVideoCodecName(KasmVideoCodecId id) {
        switch (id) {
            case kasmVideoH264:
                return "H264";
            case kasmVideoH265:
                return "H265";
            case kasmVideoAV1:
                return "AV1";
            default:
                return "unknown";
        }
    }

    static constexpr int MAX_VBV_BITS = 15'000'000;

    static constexpr auto dri_node_paths = std::to_array<const char *>({
        "renderD128",
        "renderD129"
    });
} // namespace rfb

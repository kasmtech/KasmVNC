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

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <rfb/PixelBuffer.h>
#include "rfb/Encoder.h"

namespace rfb {
    struct VideoEncoderParams {
        int width{};
        int height{};
        uint8_t frame_rate{};
        uint8_t group_of_picture{};
        uint8_t quality{};
        int max_bitrate{};

        bool operator==(const VideoEncoderParams &rhs) const noexcept {
            return width == rhs.width && height == rhs.height && frame_rate == rhs.frame_rate &&
                   group_of_picture == rhs.group_of_picture && quality == rhs.quality && max_bitrate == rhs.max_bitrate;
        }
        bool operator!=(const VideoEncoderParams &rhs) const noexcept {
            return !(*this == rhs);
        }
    };

    class VideoEncoder : public Encoder {
    public:
        VideoEncoder(Id id, SConnection *conn) :
            Encoder(id, conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1) {}
        virtual bool render(const PixelBuffer *pb, bool forceKeyFrame = false) = 0;
        virtual void writeSkipRect() = 0;
        ~VideoEncoder() override = default;

        void setMaxBitrate(int bps) {
            maxBitrateBps.store(bps, std::memory_order_relaxed);
        }
        int maxBitrate() const {
            return maxBitrateBps.load(std::memory_order_relaxed);
        }

        // Readiness flips asynchronously on the WebRTC thread, so it is sampled once per frame
        // and the keyframe decision and routeEncodedRect() see the same reading.
        bool snapshotRouting(uint8_t screenId) {
            routedOverWebRTC = isRoutedOverWebRTC(screenId);
            return routedOverWebRTC;
        }

    protected:
        bool isRoutedOverWebRTC(uint8_t screenId);

        void routeEncodedRect(uint8_t screenId, const uint8_t *data, size_t size, bool isKeyframe,
            uint8_t codecId, uint8_t codecTypeId);
        void writeVideoSkipRect(uint8_t screenId);

    private:
        std::atomic<int> maxBitrateBps{0};
        bool routedOverWebRTC{};
    };
} // namespace rfb

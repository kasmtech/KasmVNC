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
#include <array>
#include <chrono>
#include <tbb/task_arena.h>
#include <vector>
#include "KasmVideoConstants.h"
#include "VideoEncoder.h"
#include "rfb/Encoder.h"
#include "rfb/ffmpeg.h"

inline constexpr uint8_t MAX_SCREENS = 8;

namespace rfb {
    template<uint8_t T = MAX_SCREENS>
    class ScreenEncoderManager final : public Encoder {
        using mask_t = uint64_t;
        static_assert(
            T <= std::numeric_limits<mask_t>::digits, "ScreenEncoderManager mask should be changed as current mask supports T <= 64");
        struct screen_t {
            Screen layout{};
            VideoEncoder *encoder{};
            bool dirty{};
            std::chrono::steady_clock::time_point last_encode{};
        };

        static constexpr std::chrono::milliseconds idleKeyframeThreshold{500};

        // A keyframe is only worth forcing when a WebRTC peer can have missed the last one.
        [[nodiscard]] static bool idleKeyframeDue(std::chrono::steady_clock::time_point last_encode,
            std::chrono::steady_clock::time_point now, bool routed) {
            return routed && last_encode.time_since_epoch().count() != 0 && (now - last_encode) > idleKeyframeThreshold;
        }

        uint8_t count{};
        mask_t mask{};
        mask_t force_keyframe_mask{};
        std::vector<uint8_t> screens_to_refresh;

        std::array<screen_t, T> screens{};
        tbb::task_arena arena;
        const FFmpeg &ffmpeg;
        VideoEncoderParams current_params;

        KasmVideoEncoders::EncoderConfig base_video_encoder;
        KasmVideoEncoders::EncoderConfigs available_encoders;

        std::array<int, T> max_bitrate{};

        [[nodiscard]] VideoEncoder *add_encoder(const Screen &layout) const;
        bool add_screen(uint8_t index, const Screen &layout);
        [[nodiscard]] size_t get_screen_count() const;
        void remove_screen(uint8_t index);
        void rebuild_screens_to_refresh();
        void clear_screens(mask_t clear_mask);

    public:
        struct stats_t {
            uint64_t rects{};
            uint64_t pixels{};
            uint64_t bytes{};
            uint64_t equivalent{};
        };
        [[nodiscard]] stats_t get_stats() const;
        // Iterator
        using iterator = typename std::array<screen_t, T>::iterator;
        using const_iterator = typename std::array<screen_t, T>::const_iterator;

        iterator begin() {
            return screens.begin();
        }
        iterator end() {
            return screens.end();
        }

        [[nodiscard]] const_iterator cbegin() const {
            return screens.begin();
        }
        [[nodiscard]] const_iterator cend() const {
            return screens.end();
        }

        explicit ScreenEncoderManager(const FFmpeg &ffmpeg_, const KasmVideoEncoders::EncoderConfig &encoder,
            const KasmVideoEncoders::EncoderConfigs &encoders, SConnection *conn, VideoEncoderParams params);
        ~ScreenEncoderManager() override;

        ScreenEncoderManager(const ScreenEncoderManager &) = delete;
        ScreenEncoderManager &operator=(const ScreenEncoderManager &) = delete;
        ScreenEncoderManager(ScreenEncoderManager &&) = delete;
        ScreenEncoderManager &operator=(ScreenEncoderManager &&) = delete;

        void clear();
        void set_params(const KasmVideoEncoders::EncoderConfig &encoder, const KasmVideoEncoders::EncoderConfigs &encoders,
            VideoEncoderParams params);

        bool sync_layout(const ScreenSet &layout, const Region &region);
        [[nodiscard]] KasmVideoEncoders::EncoderConfig get_encoder_config() const {
            return base_video_encoder;
        }

        void force_keyframe(uint8_t index) {
            if (index < T)
                force_keyframe_mask |= mask_t{1} << index;
        }

        void set_max_bitrate(uint8_t index, int bps) {
            if (index >= T)
                return;
            max_bitrate[index] = bps;
            if (screens[index].encoder)
                screens[index].encoder->setMaxBitrate(bps);
        }

        // Encoder
        [[nodiscard]] bool isSupported() const override;

        bool writeFrame(const PixelBuffer *pb, const Palette &palette, bool forceKeyFrame = false);
        void writeRect(const PixelBuffer *pb, const Palette &palette) override {}
        void writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) override;

    private:
        stats_t stats{};
    };

    template class ScreenEncoderManager<>;
} // namespace rfb

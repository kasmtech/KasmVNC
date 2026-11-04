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

#include <vector>
#include <span>
#include "SupportedVideoEncoders.h"
#include "rfb/ffmpeg.h"

namespace rfb::video_encoders {
    class EncoderProbe {
        KasmVideoEncoders::EncoderConfig best_encoder{KasmVideoEncoders::Encoder::h264_software};
        KasmVideoEncoders::EncoderConfigs available_encoder_configs;
        FFmpeg &ffmpeg;

        struct EncoderCandidate {
            KasmVideoEncoders::Encoder encoder;
            FFmpeg::Device hw_type;
        };

        explicit EncoderProbe(FFmpeg &ffmpeg, const std::vector<std::string_view> &parsed_encoders, const char *dri_node);
        static bool dri_node_supports_hwdevice_type(const char *dri_node, FFmpeg::Device hw_type);
        bool try_open_codec(const char *dri_node, const EncoderCandidate &candidate) const;
        KasmVideoEncoders::EncoderConfigs probe(const char *dri_node, std::span<EncoderCandidate> candidates);

    public:
        EncoderProbe(const EncoderProbe &) = delete;
        EncoderProbe &operator=(const EncoderProbe &) = delete;
        EncoderProbe(EncoderProbe &&) = delete;
        EncoderProbe &operator=(EncoderProbe &&) = delete;

        static EncoderProbe &get(FFmpeg &ffmpeg, const std::vector<std::string_view> &parsed_encoders, const char *dri_node) {
            static EncoderProbe instance{ffmpeg, parsed_encoders, dri_node};

            return instance;
        }

        // [[nodiscard]] static bool is_acceleration_available();

        [[nodiscard]] KasmVideoEncoders::EncoderConfig get_best_encoder() const {
            return best_encoder;
        }

        [[nodiscard]] const KasmVideoEncoders::EncoderConfigs &get_available_encoders() const {
            return available_encoder_configs;
        }

        /*[[nodiscard]] const KasmVideoEncoders::Encoders &update_encoders(const std::vector<std::string_view> &codecs) {
            available_encoders = SupportedVideoEncoders::filter_available_encoders(SupportedVideoEncoders::map_encoders(codecs), available_encoders);
            return available_encoders;
        }*/
    };

} // namespace rfb::video_encoders

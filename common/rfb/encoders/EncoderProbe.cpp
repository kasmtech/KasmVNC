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
#include "EncoderProbe.h"
#include <algorithm>
#include <fcntl.h>
#include <fmt/format.h>
#include <fstream>
#include <rfb/LogWriter.h>
#include <string>
#include <unistd.h>
#include <vector>
#include "KasmVideoConstants.h"
#include "KasmVideoEncoders.h"

#include "rfb/ffmpeg.h"
#include "EncoderConfiguration.h"

namespace rfb::video_encoders {
    static LogWriter vlog("EncoderProbe");

    EncoderProbe::EncoderProbe(FFmpeg &ffmpeg_, const std::vector<std::string_view> &parsed_encoders, const char *dri_node) :
        ffmpeg(ffmpeg_) {
        if (!ffmpeg.is_available()) {
            available_encoder_configs.emplace_back(KasmVideoEncoders::Encoder::unavailable);
        } else {
            auto debug_encoders = []<typename T, typename Alloc = std::allocator<T>>(const char *msg, const std::vector<T, Alloc> &encoders) {
                std::string encoder_names;

                for (const auto &encoder: encoders) {
                    std::string encoder_name;

                    if constexpr (std::is_same_v<T, KasmVideoEncoders::Encoder>)
                        encoder_name = KasmVideoEncoders::to_string(encoder);
                    else
                        encoder_name = KasmVideoEncoders::to_string(encoder.encoder);

                    encoder_names.append(encoder_name).append(" ");
                }

                if (!encoder_names.empty())
                    vlog.debug("%s: %s", msg, encoder_names.c_str());
            };

            const auto encoders = SupportedVideoEncoders::map_encoders(parsed_encoders);

            if (dri_node)
                vlog.debug("Dri node: %s", dri_node);

            debug_encoders("CLI-specified video codecs", encoders);

            static std::array<EncoderCandidate, 8> candidates = {
                {
                    EncoderCandidate{KasmVideoEncoders::Encoder::h264_nvenc, FFmpeg::Device::CUDA},
                    EncoderCandidate{KasmVideoEncoders::Encoder::h265_nvenc, FFmpeg::Device::CUDA},
                    EncoderCandidate{KasmVideoEncoders::Encoder::av1_nvenc, FFmpeg::Device::CUDA},
                    EncoderCandidate{KasmVideoEncoders::Encoder::h264_ffmpeg_vaapi, FFmpeg::Device::VAAPI},
                    EncoderCandidate{KasmVideoEncoders::Encoder::h265_ffmpeg_vaapi, FFmpeg::Device::VAAPI},
                    EncoderCandidate{KasmVideoEncoders::Encoder::av1_ffmpeg_vaapi, FFmpeg::Device::VAAPI},
                    EncoderCandidate{KasmVideoEncoders::Encoder::h264_software, FFmpeg::Device::Software},
                    EncoderCandidate{KasmVideoEncoders::Encoder::h265_software, FFmpeg::Device::Software}
                    // EncoderCandidate{KasmVideoEncoders::Encoder::av1_software, FFmpeg::Device::Software},
                }
            };

            available_encoder_configs = probe(dri_node, candidates);

            debug_encoders("Available encoders", available_encoder_configs);

            available_encoder_configs = SupportedVideoEncoders::filter_available_encoders(encoders, available_encoder_configs);
            debug_encoders("Using CLI-specified video codecs (supported subset)", available_encoder_configs);
        }

        available_encoder_configs.shrink_to_fit();
        if (available_encoder_configs.empty())
            best_encoder = {KasmVideoEncoders::Encoder::unavailable};
        else
            best_encoder = available_encoder_configs.front();
    }

    bool EncoderProbe::dri_node_supports_hwdevice_type(const char *dri_node, FFmpeg::Device hw_type) {
        const auto path = fmt::format("/sys/class/drm/{}/device/vendor", dri_node);

        std::ifstream file{path};
        if (!file.is_open())
            return false;

        std::string s;
        file >> s;
        uint32_t vendor_id = std::stoul(s, nullptr, 0);

        switch (hw_type) {
            case FFmpeg::Device::CUDA:
                return vendor_id == 0x10DE;
            case FFmpeg::Device::VAAPI:
                return vendor_id == 0x8086 || vendor_id == 0x1002;
            default:
                return false;
        }
    }

    bool EncoderProbe::try_open_codec(const char *device_path, const EncoderCandidate &candidate) const {
        const auto &config = EncoderConfiguration::get_configuration(candidate.encoder);
        FFmpeg::EncoderSettings settings{KasmVideoEncoders::to_string(candidate.encoder), candidate.hw_type,
            device_path ? device_path : "", 1920, 1080, 60, 30, 25, config.profile, config.allowed_quality.max};
        return ffmpeg.probe_encoder(settings);
    }

    KasmVideoEncoders::EncoderConfigs EncoderProbe::probe(
        const char *dri_node, std::span<EncoderCandidate> candidates) {
        KasmVideoEncoders::EncoderConfigs result{};
        for (const auto &encoder_candidate: candidates) {
            const auto *name = KasmVideoEncoders::to_string(encoder_candidate.encoder);
            vlog.debug("Probing %s...", name);
            if (encoder_candidate.hw_type == FFmpeg::Device::Software) {
                if (ffmpeg.has_encoder(name))
                    result.emplace_back(encoder_candidate.encoder);
                else
                    vlog.info("Software encoder %s is not provided by this FFmpeg runtime", name);
                continue;
            }
            // CUDA selects a device ordinal, not a DRM render-node pathname.
            if (encoder_candidate.hw_type == FFmpeg::Device::CUDA) {
                if (try_open_codec(nullptr, encoder_candidate)) result.emplace_back(encoder_candidate.encoder);
                continue;
            }

            if (dri_node) {
                if (try_open_codec(dri_node, encoder_candidate)) {
                    result.emplace_back(encoder_candidate.encoder, dri_node);
                }
            } else {
                vlog.debug("Trying to probe all available dri nodes");
                for (const auto *dri_node_path: dri_node_paths) {
                    vlog.debug("Probing dri node %s", dri_node_path);
                    if (!dri_node_supports_hwdevice_type(dri_node_path, encoder_candidate.hw_type)) {
                        vlog.debug("Skipping dri node %s", dri_node_path);
                        continue;
                    }

                    const auto dri_device = fmt::format("/dev/dri/{}", dri_node_path);
                    vlog.debug("Trying dri node %s", dri_device.c_str());
                    if (!try_open_codec(dri_device.c_str(), encoder_candidate))
                        continue;

                    result.emplace_back(encoder_candidate.encoder, dri_device);
                }
            }
        }

        // result.push_back(KasmVideoEncoders::Encoder::av1_software);

        const bool has_software = std::any_of(result.begin(), result.end(), [](const auto &config) {
            return config.encoder == KasmVideoEncoders::Encoder::h264_software ||
                   config.encoder == KasmVideoEncoders::Encoder::h265_software;
        });
        if (!has_software)
            vlog.error("No software video encoder (libx264/libx265) in this FFmpeg runtime; "
                       "hardware encoder failures cannot fall back to software");

        return result;
    }

    /*bool EncoderProbe::is_acceleration_available() {
        if (access(render_path, R_OK | W_OK) != 0)
            return false;

        const int fd = open(render_path, O_RDWR);
        if (fd < 0)
            return false;

        close(fd);

        return true;
    }*/
} // namespace rfb::video_encoders

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
#include "ScreenEncoderManager.h"
#include <cassert>
#include <rfb/webrtc/WebRTCMediaTransport.h>
#include <rfb/LogWriter.h>
#include <rfb/Region.h>
#include <rfb/SConnection.h>
#include <rfb/SMsgWriter.h>
#include <rfb/encodings.h>
#include <sys/stat.h>
#include <atomic>
#include <tbb/parallel_for_each.h>
#include "VideoEncoder.h"
#include "VideoEncoderFactory.h"

namespace rfb {
    static LogWriter vlog("ScreenEncoderManager");

    template<uint8_t T>
    ScreenEncoderManager<T>::ScreenEncoderManager(const FFmpeg &ffmpeg_, const KasmVideoEncoders::EncoderConfig &encoder,
        const KasmVideoEncoders::EncoderConfigs &encoders, SConnection *conn, VideoEncoderParams params) :
        Encoder(conn, encodingKasmVideo, static_cast<EncoderFlags>(EncoderUseNativePF | EncoderLossy), -1),
        ffmpeg(ffmpeg_),
        current_params(params),
        base_video_encoder(encoder),
        available_encoders(encoders) {
        screens_to_refresh.reserve(T);
        arena.initialize(MAX_SCREENS);
    }

    template<uint8_t T>
    ScreenEncoderManager<T>::~ScreenEncoderManager() {
        clear_screens(mask);
    }

    template<uint8_t T>
    void ScreenEncoderManager<T>::clear() {
        current_params = {};
        base_video_encoder = KasmVideoEncoders::EncoderConfig{KasmVideoEncoders::Encoder::unavailable};
        available_encoders.clear();
        screens_to_refresh.clear();
        clear_screens(mask);
    }

    template<uint8_t T>
    void ScreenEncoderManager<T>::set_params(const KasmVideoEncoders::EncoderConfig &encoder,
        const KasmVideoEncoders::EncoderConfigs &encoders, VideoEncoderParams params) {
        base_video_encoder = encoder;
        available_encoders = encoders;
        current_params = params;
    }


    template<uint8_t T>
    VideoEncoder *ScreenEncoderManager<T>::add_encoder(const Screen &layout) const {
        VideoEncoder *encoder{};
        try {
            encoder = create_encoder(layout, &ffmpeg, conn, base_video_encoder.encoder, base_video_encoder.dri_path.c_str(), current_params);
        } catch (const std::exception &e) {
            if (base_video_encoder.encoder != KasmVideoEncoders::Encoder::h264_software) {
                vlog.error("Attempting fallback to software encoder due to error: %s", e.what());
                try {
                    encoder = create_encoder(layout, &ffmpeg, conn, KasmVideoEncoders::Encoder::h264_software, nullptr, current_params);
                } catch (const std::exception &exception) {
                    vlog.error("Failed to create software encoder: %s", exception.what());
                }
            } else
                vlog.error("Failed to create software encoder: %s", e.what());
        }

        return encoder;
    }

    template<uint8_t T>
    bool ScreenEncoderManager<T>::add_screen(uint8_t index, const Screen &layout) {
        screens[index] = {layout, nullptr, true};
        screens[index].layout.id = index;
        screens[index].encoder = add_encoder(screens[index].layout);

        if (!screens[index].encoder) {
            screens[index] = {};
            return false;
        }

        screens[index].encoder->setMaxBitrate(max_bitrate[index]);

        mask |= 1ULL << index;

        ++count;

        return true;
    }

    template<uint8_t T>
    size_t ScreenEncoderManager<T>::get_screen_count() const {
        return count;
    }

    template<uint8_t T>
    void ScreenEncoderManager<T>::remove_screen(uint8_t index) {
        if (screens[index].encoder) {
            delete screens[index].encoder;
            screens[index].encoder = nullptr;

            --count;
        }
        mask &= ~(1ULL << index);
        screens[index] = {};
    }

    template<uint8_t T>
    void ScreenEncoderManager<T>::rebuild_screens_to_refresh() {
        screens_to_refresh.clear();

        mask_t remaining_mask = mask;
        while (remaining_mask) {
            const auto pos = __builtin_ctzll(remaining_mask);
            if (screens[pos].dirty)
                screens_to_refresh.push_back(pos);

            remaining_mask &= remaining_mask - 1;
        }
    }

    template<uint8_t T>
    void ScreenEncoderManager<T>::clear_screens(mask_t clear_mask) {
        mask_t remaining_mask = clear_mask;
        while (remaining_mask) {
            const auto pos = __builtin_ctzll(remaining_mask);
            remove_screen(pos);
            remaining_mask &= remaining_mask - 1;
        }
    }

    template<uint8_t T>
    ScreenEncoderManager<T>::stats_t ScreenEncoderManager<T>::get_stats() const {
        return stats;
    }

    template<uint8_t T>
    bool ScreenEncoderManager<T>::sync_layout(const ScreenSet &layout, const Region &region) {
        const auto old_mask = mask;
        mask_t new_mask = 0;
        unsigned overflow = 0;

        const unsigned n = static_cast<unsigned>(layout.num_screens());
        const unsigned tcap = static_cast<unsigned>(T);
        const uint8_t n_capped = static_cast<uint8_t>(n > tcap ? tcap : n);

        for (uint8_t slot = 0; slot < n_capped; ++slot) {
            const auto &screen = layout.screens[slot];

            const bool was_present = (old_mask & (mask_t{1} << slot)) != 0;
            const bool geom_changed = !screens[slot].layout.dimensions.equals(screen.dimensions);

            new_mask |= mask_t{1} << slot;

            if (!was_present || geom_changed) {
                if (was_present)
                    remove_screen(slot);
                if (!add_screen(slot, screen)) {
                    return false;
                }
            } else if (!region.intersect(Region(screen.dimensions)).is_empty()) {
                screens[slot].dirty = true;
            }
        }

        if (n > tcap) {
            overflow = n - tcap;
            vlog.error("%u screen(s) exceed T=%u — ignored (max %u monitors)",
                overflow,
                static_cast<unsigned>(T),
                static_cast<unsigned>(T));
        }

        if (const auto stale_screens = old_mask & ~new_mask; stale_screens) {
            mask_t rem = stale_screens;
            while (rem) {
                const uint8_t s = static_cast<uint8_t>(__builtin_ctzll(rem));
                remove_screen(s);
                rem &= rem - 1;
            }
        }

        //if (old_mask != mask || (mask > 0 && screens_to_refresh.empty()))
            rebuild_screens_to_refresh();

        return true;
    }

    template<uint8_t T>
    bool ScreenEncoderManager<T>::isSupported() const {
        const auto index = screens_to_refresh[0];
        if (const auto *encoder = screens[index].encoder; encoder)
            return encoder->isSupported();

        return false;
    }

    template<uint8_t T>
    bool ScreenEncoderManager<T>::writeFrame(const PixelBuffer *pb, const Palette &palette, bool forceKeyFrame) {
        if (screens_to_refresh.empty())
            return true;

        const auto bpp = conn->cp.pf().bpp >> 3;
        auto *out_conn = conn->getOutStream();

        if (!out_conn) {
            vlog.error("writeRect: getOutStream returned NULL");
            return false;
        }

        const auto send_frame = [this, &bpp, out_conn, pb, &palette](uint8_t index, screen_t &screen) {
            // Cleared before writing, so the bit only ever covers the frame being sent.
            force_keyframe_mask &= ~(mask_t{1} << index);

            ++stats.rects;
            const auto &rect = screen.layout.dimensions;
            const auto area = rect.area();
            stats.pixels += area;
            const auto before = out_conn->length();

            const int equiv = 12 + (area * bpp);
            stats.equivalent += equiv;

            const auto &encoder = screen.encoder;

            conn->writer()->startRect(rect, encoder->encoding);
            encoder->writeRect(pb, palette);
            conn->writer()->endRect();

            screen.dirty = false;

            const auto after = out_conn->length();
            stats.bytes += after - before;
        };

        // Snapshot only; a screen's bit is cleared when its frame is sent, so a failed render keeps the request.
        const mask_t kf_mask = force_keyframe_mask;
        const auto now = std::chrono::steady_clock::now();
        const auto wants_keyframe = [&](uint8_t index) {
            const auto &screen = screens[index];
            const bool routed = screen.encoder->snapshotRouting(index);
            return forceKeyFrame || (kf_mask & (mask_t{1} << index)) || idleKeyframeDue(screen.last_encode, now, routed);
        };

        // Nothing from a previous tick may survive into this one.
        for (auto index: screens_to_refresh)
            screens[index].dirty = false;

        if (screens_to_refresh.size() > 1) {
            std::atomic<bool> failed{false};
            arena.execute([&] {
                tbb::parallel_for_each(screens_to_refresh.begin(),
                    screens_to_refresh.end(),
                    [this, pb, now, &wants_keyframe, &failed](uint8_t index) {
                        auto &screen = screens[index];
                        if (auto *encoder = screen.encoder; encoder) {
                            screen.dirty = encoder->render(pb, wants_keyframe(index));
                            if (screen.dirty)
                                screen.last_encode = now;
                            else
                                failed.store(true, std::memory_order_relaxed);
                        }
                    });
            });

            if (failed.load(std::memory_order_relaxed)) {
                for (auto index: screens_to_refresh)
                    screens[index].dirty = false;
                return false;
            }

            bool any_sent = false;
            for (auto index: screens_to_refresh) {
                auto &screen = screens[index];
                if (screen.encoder && screen.dirty) {
                    send_frame(index, screen);
                    any_sent = true;
                }
            }
            if (!any_sent)
                return false;
        } else {
            const auto index = screens_to_refresh[0];
            if (auto encoder = screens[index].encoder; encoder) {
                if (encoder->render(pb, wants_keyframe(index))) {
                    screens[index].last_encode = now;
                    screens[index].dirty = true;
                    send_frame(index, screens[index]);
                } else
                    return false;
            }
        }

        return true;
    }

    template<uint8_t T>
    void ScreenEncoderManager<T>::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {
        for (const auto index: screens_to_refresh) {
            if (auto *encoder = screens[index].encoder; encoder)
                encoder->writeSolidRect(width, height, pf, colour);
        }
    }

} // namespace rfb

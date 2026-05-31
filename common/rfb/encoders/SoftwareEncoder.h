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

#include "KasmVideoConstants.h"
#include "rdr/OutStream.h"
#include "rfb/Encoder.h"
#include "rfb/encoders/VideoEncoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
    class SoftwareEncoder final : public VideoEncoder {
        Screen layout;
        const FFmpeg &ffmpeg;
        const AVCodec *codec{};

        FFmpeg::FrameGuard frame_guard;
        FFmpeg::PacketGuard pkt_guard;
        FFmpeg::ContextGuard ctx_guard;

        KasmVideoEncoders::Encoder encoder;
        VideoEncoderParams current_params{};
        uint8_t msg_codec_id;
        uint8_t msg_codec_type_id;

        int64_t pts{};
        int bpp{};
        [[nodiscard]] bool init(int width, int height, VideoEncoderParams params);

        template<typename T>
        friend class EncoderBuilder;
        SoftwareEncoder(Screen layout, const FFmpeg &ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder encoder,
                            VideoEncoderParams params);
    public:
        bool isSupported() const override;
        void writeRect(const PixelBuffer *pb, const Palette &palette) override;
        void writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) override;
        bool render(const PixelBuffer *pb, bool forceKeyFrame = false) override;
        void writeSkipRect() override;
    };
} // namespace rfb

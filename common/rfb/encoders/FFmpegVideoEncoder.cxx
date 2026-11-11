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
#include "FFmpegVideoEncoder.h"
#include <stdexcept>
#include "EncoderConfiguration.h"
#include "rfb/LogWriter.h"
#include "rfb/ServerCore.h"
#include "rfb/encoders/utils.h"

static rfb::LogWriter vlog("FFmpegVideoEncoder");

static const char *kind(FFmpeg::Device device) {
    return device == FFmpeg::Device::Software ? "Software" : "Hardware";
}

namespace rfb {
    FFmpegVideoEncoder::FFmpegVideoEncoder(Screen layout_, const FFmpeg &ffmpeg_, SConnection *conn,
                                           KasmVideoEncoders::Encoder encoder_, VideoEncoderParams params,
                                           FFmpeg::Device device_, const char *path) : VideoEncoder(layout_.id, conn),
        layout(layout_),
        ffmpeg(ffmpeg_),
        encoder(encoder_),
        device(device_),
        device_path(path ? path : ""),
        msg_codec_id(KasmVideoEncoders::to_msg_id(encoder)),
        msg_codec_type_id(
            pseudoEncodingStreamingModeJpegWebp -
            KasmVideoEncoders::to_encoding(encoder)) {
        vlog.debug("%s %s: Constructor: device=%s", kind(device), KasmVideoEncoders::to_string(encoder),
                   device_path.c_str());
        if (!ffmpeg.is_available())
            throw std::runtime_error("FFmpeg is not available");
        params.width = layout.dimensions.width() & ~1;
        params.height = layout.dimensions.height() & ~1;
        if (!params.frame_rate)
            params.frame_rate = Server::frameRate;
        init(params);
    }

    void FFmpegVideoEncoder::init(VideoEncoderParams params) {
        const auto &config = EncoderConfiguration::get_configuration(encoder);
        FFmpeg::EncoderSettings settings{
            KasmVideoEncoders::to_string(encoder),
            device,
            device_path,
            params.width,
            params.height,
            params.frame_rate,
            params.group_of_picture,
            params.quality,
            config.profile,
            config.allowed_quality.max,
            backend ? backend->next_pts() : 0
        };
        auto next = ffmpeg.create_encoder(settings);
        backend = std::move(next);
        current_params = params;
        vlog.debug("%s %s: FRAME RESIZE (%d, %d): RATE: %d, GOP: %d, QUALITY: %d",
                   kind(device),
                   KasmVideoEncoders::to_string(encoder),
                   params.width,
                   params.height,
                   params.frame_rate,
                   params.group_of_picture,
                   params.quality);
    }

    bool FFmpegVideoEncoder::isSupported() const {
        return conn->cp.supportsEncoding(encodingKasmVideo);
    }

    bool FFmpegVideoEncoder::render(const PixelBuffer *pb, bool forceKeyFrame) {
        try {
            const auto rect = layout.dimensions;
            VideoEncoderParams params{
                rect.width() & ~1,
                rect.height() & ~1,
                static_cast<uint8_t>(Server::frameRate),
                static_cast<uint8_t>(Server::groupOfPicture),
                static_cast<uint8_t>(Server::videoQualityCRFCQP)
            };
            const bool reinit = params != current_params;
            if (reinit || !bpp) {
                const int bytes = pb->getPF().bpp >> 3;
                if (bytes != 4)
                    throw std::runtime_error("Video encoder requires a 32-bit framebuffer");
                bpp = bytes;
            }
            if (reinit) {
                init(params);
                forceKeyFrame = true;
            }
            int stride{};
            const auto *pixels = pb->getBuffer(rect, &stride);
            backend->encode_argb(pixels, stride * bpp, forceKeyFrame);
            const auto packet = backend->packet();
            if (packet.keyframe)
                DEBUG_LOG(vlog, "%s %s: Key frame %ld", kind(device), KasmVideoEncoders::to_string(encoder),
                          packet.pts);
            return true;
        } catch (const std::exception &e) {
            vlog.error("%s %s: video encoding failed: %s", kind(device), KasmVideoEncoders::to_string(encoder),
                       e.what());
            return false;
        }
    }

    void FFmpegVideoEncoder::writeRect(const PixelBuffer *, const Palette &) {
        const auto packet = backend->packet();
        if (!packet.data) {
            writeSkipRect();
            return;
        }
        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(layout.id);
        os->writeU8(msg_codec_id);
        os->writeU8(msg_codec_type_id);
        os->writeU8(packet.keyframe);
        encoders::write_compact(os, packet.size);
        os->writeBytes(packet.data, packet.size);
        DEBUG_LOG(vlog, "%s %s: Screen id %d, codec %d, frame size: %d", kind(device),
                  KasmVideoEncoders::to_string(encoder), layout.id, msg_codec_id, int(packet.size));
        backend->pop_packet();
    }

    void FFmpegVideoEncoder::writeSkipRect() {
        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(layout.id);
        os->writeU8(kasmVideoSkip);
    }
} // namespace rfb

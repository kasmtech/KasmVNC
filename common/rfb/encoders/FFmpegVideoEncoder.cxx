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
#include <fmt/format.h>
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
        if (!init(params))
            throw std::runtime_error(fmt::format("Cannot create {}", KasmVideoEncoders::to_string(encoder)));
    }

    bool FFmpegVideoEncoder::init(VideoEncoderParams params) {
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
            backend ? backend->next_pts() : 0,
            params.max_bitrate,
            MAX_VBV_BITS
        };
        auto next = ffmpeg.create_encoder(settings);
        if (!next) {
            vlog.error("%s %s: cannot create encoder", kind(device), KasmVideoEncoders::to_string(encoder));
            return false;
        }
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
        return true;
    }

    bool FFmpegVideoEncoder::isSupported() const {
        return conn->cp.supportsEncoding(encodingKasmVideo);
    }

    bool FFmpegVideoEncoder::render(const PixelBuffer *pb, bool forceKeyFrame) {
        const auto rect = layout.dimensions;
        VideoEncoderParams params{
            rect.width() & ~1,
            rect.height() & ~1,
            static_cast<uint8_t>(Server::frameRate),
            static_cast<uint8_t>(Server::groupOfPicture),
            static_cast<uint8_t>(Server::videoQualityCRFCQP),
            maxBitrate()
        };
        const bool reinit = params != current_params;
        if (reinit || !bpp) {
            const int bytes = pb->getPF().bpp >> 3;
            if (bytes != 4) {
                vlog.error("%s %s: video encoder requires a 32-bit framebuffer, got %d bpp", kind(device),
                           KasmVideoEncoders::to_string(encoder), pb->getPF().bpp);
                return false;
            }
            bpp = bytes;
        }
        if (reinit) {
            if (!init(params))
                return false;
            forceKeyFrame = true;
        }
        int stride{};
        const auto *pixels = pb->getBuffer(rect, &stride);
        if (!backend->encode_argb(pixels, stride * bpp, forceKeyFrame))
            return false;

        const auto packet = backend->packet();
        if (packet.keyframe)
            DEBUG_LOG(vlog, "%s %s: Key frame %ld", kind(device), KasmVideoEncoders::to_string(encoder), packet.pts);
        return true;
    }

    void FFmpegVideoEncoder::writeRect(const PixelBuffer *, const Palette &) {
        const auto packet = backend->packet();
        if (!packet.data) {
            writeSkipRect();
            return;
        }
        routeEncodedRect(layout.id, packet.data, packet.size, packet.keyframe, msg_codec_id, msg_codec_type_id);
        DEBUG_LOG(vlog, "%s %s: Screen id %d, codec %d, frame size: %d", kind(device),
                  KasmVideoEncoders::to_string(encoder), layout.id, msg_codec_id, int(packet.size));
        backend->pop_packet();
    }

    void FFmpegVideoEncoder::writeSkipRect() {
        writeVideoSkipRect(layout.id);
    }
} // namespace rfb

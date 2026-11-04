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
#include "SoftwareEncoder.h"
#include "EncoderConfiguration.h"
#include "rfb/LogWriter.h"
#include "rfb/ServerCore.h"
#include "rfb/encoders/utils.h"
#include <stdexcept>

static rfb::LogWriter software_log("SoftwareEncoder");
static rfb::LogWriter hardware_log("FFMPEGHWEncoder");
namespace rfb {
SoftwareEncoder::SoftwareEncoder(Screen layout_, const FFmpeg &ffmpeg_, SConnection *conn,
    KasmVideoEncoders::Encoder encoder_, VideoEncoderParams params)
    : SoftwareEncoder(layout_, ffmpeg_, conn, encoder_, params, FFmpeg::Device::Software, nullptr) {}

SoftwareEncoder::SoftwareEncoder(Screen layout_, const FFmpeg &ffmpeg_, SConnection *conn,
    KasmVideoEncoders::Encoder encoder_, VideoEncoderParams params, FFmpeg::Device device_, const char *path)
    : VideoEncoder(layout_.id, conn), layout(layout_), ffmpeg(ffmpeg_), encoder(encoder_), device(device_),
      device_path(path ? path : ""), msg_codec_id(KasmVideoEncoders::to_msg_id(encoder)),
      msg_codec_type_id(pseudoEncodingStreamingModeJpegWebp - KasmVideoEncoders::to_encoding(encoder)) {
    auto &vlog = device == FFmpeg::Device::Software ? software_log : hardware_log;
    vlog.debug("Constructor: encoder=%s, device=%s", KasmVideoEncoders::to_string(encoder), device_path.c_str());
    if (!ffmpeg.is_available()) throw std::runtime_error("FFmpeg is not available");
    params.width = layout.dimensions.width() & ~1;
    params.height = layout.dimensions.height() & ~1;
    if (!params.frame_rate) params.frame_rate = Server::frameRate;
    init(params);
}
void SoftwareEncoder::init(VideoEncoderParams params) {
    auto &vlog = device == FFmpeg::Device::Software ? software_log : hardware_log;
    const auto &config = EncoderConfiguration::get_configuration(encoder);
    FFmpeg::EncoderSettings settings{KasmVideoEncoders::to_string(encoder), device, device_path,
        params.width, params.height, params.frame_rate, params.group_of_picture, params.quality,
        config.profile, config.allowed_quality.max};
    auto replacement = ffmpeg.create_encoder(settings);
    backend = std::move(replacement);
    current_params = params;
    vlog.debug("FRAME RESIZE (%d, %d): RATE: %d, GOP: %d, QUALITY: %d", params.width, params.height,
               params.frame_rate, params.group_of_picture, params.quality);
}
bool SoftwareEncoder::isSupported() const { return conn->cp.supportsEncoding(encodingKasmVideo); }
bool SoftwareEncoder::render(const PixelBuffer *pb, bool forceKeyFrame) {
    auto &vlog = device == FFmpeg::Device::Software ? software_log : hardware_log;
    try {
        const auto rect = layout.dimensions;
        VideoEncoderParams params{rect.width() & ~1, rect.height() & ~1,
            static_cast<uint8_t>(Server::frameRate), static_cast<uint8_t>(Server::groupOfPicture),
            static_cast<uint8_t>(Server::videoQualityCRFCQP)};
        if (params != current_params) { init(params); forceKeyFrame = true; }
        int stride{};
        const auto *pixels = pb->getBuffer(rect, &stride);
        const int bytes = pb->getPF().bpp >> 3;
        if (bytes != 4) throw std::runtime_error("Video encoder requires a 32-bit framebuffer");
        backend->encode_argb(pixels, stride * bytes, pts++, forceKeyFrame);
        const auto packet = backend->packet();
        if (packet.keyframe) DEBUG_LOG(vlog, "Key frame %ld", packet.pts);
        return true;
    } catch (const std::exception &e) {
        vlog.error("Video encoding failed: %s", e.what());
        return false;
    }
}
void SoftwareEncoder::writeRect(const PixelBuffer *, const Palette &) {
    auto &vlog = device == FFmpeg::Device::Software ? software_log : hardware_log;
    const auto packet = backend->packet();
    if (!packet.data) { writeSkipRect(); return; }
    auto *os = conn->getOutStream(conn->cp.supportsUdp);
    os->writeU8(layout.id);
    os->writeU8(msg_codec_id);
    os->writeU8(msg_codec_type_id);
    os->writeU8(packet.keyframe);
    encoders::write_compact(os, packet.size);
    os->writeBytes(packet.data, packet.size);
    DEBUG_LOG(vlog, "Screen id %d, codec %d, frame size: %d", layout.id, msg_codec_id, int(packet.size));
    backend->pop_packet();
}
void SoftwareEncoder::writeSkipRect() {
    auto *os = conn->getOutStream(conn->cp.supportsUdp);
    os->writeU8(layout.id);
    os->writeU8(kasmVideoSkip);
}
} // namespace rfb

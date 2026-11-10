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
#include "rfb/LogWriter.h"
#include "rfb/encoders/VideoEncoder.h"
#include "rfb/ffmpeg.h"

namespace rfb {
class FFmpegVideoEncoder : public VideoEncoder {
    Screen layout;
    const FFmpeg &ffmpeg;
    FFmpeg::EncoderSlot slots[2];
    FFmpeg::Encoder *backend{};
    unsigned active{};
    KasmVideoEncoders::Encoder encoder;
    FFmpeg::Device device;
    std::string device_path;
    LogWriter &logger;
    VideoEncoderParams current_params{};
    uint8_t msg_codec_id{}, msg_codec_type_id{};
    int64_t pts{};
    void init(VideoEncoderParams params);

protected:
    FFmpegVideoEncoder(Screen layout, const FFmpeg &ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder encoder,
        VideoEncoderParams params, FFmpeg::Device device, const char *device_path, LogWriter &logger);

public:
    bool isSupported() const override;
    void writeRect(const PixelBuffer *pb, const Palette &palette) override;
    void writeSolidRect(int, int, const PixelFormat &, const rdr::U8 *) override {}
    bool render(const PixelBuffer *pb, bool forceKeyFrame = false) override;
    void writeSkipRect() override;
};

template<FFmpeg::Device D>
struct EncoderLog {
    static LogWriter &get();
};
template<>
LogWriter &EncoderLog<FFmpeg::Device::Software>::get();
template<>
LogWriter &EncoderLog<FFmpeg::Device::VAAPI>::get();
template<>
LogWriter &EncoderLog<FFmpeg::Device::CUDA>::get();

template<FFmpeg::Device D>
class FFmpegDeviceEncoder final : public FFmpegVideoEncoder {
    template<typename T>
    friend class EncoderBuilder;

    FFmpegDeviceEncoder(Screen layout, const FFmpeg &ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder encoder,
        VideoEncoderParams params)
        requires(D == FFmpeg::Device::Software)
        :
        FFmpegVideoEncoder(layout, ffmpeg, conn, encoder, params, D, nullptr, EncoderLog<D>::get()) {}

    FFmpegDeviceEncoder(Screen layout, const FFmpeg &ffmpeg, SConnection *conn, KasmVideoEncoders::Encoder encoder,
        const char *path, VideoEncoderParams params)
        requires(D != FFmpeg::Device::Software)
        :
        FFmpegVideoEncoder(layout, ffmpeg, conn, encoder, params, D, path, EncoderLog<D>::get()) {}
};
} // namespace rfb

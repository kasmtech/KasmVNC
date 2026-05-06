/* Copyright (C) 2025 Kasm Technologies Corp
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

#include "FfmpegFrameFeeder.h"
#include <filesystem>
#include <string_view>

FfmpegFrameFeeder::FfmpegFrameFeeder(FFmpeg *ffmpeg_) : ffmpeg{ffmpeg_} {}

FfmpegFrameFeeder::~FfmpegFrameFeeder() = default;

void FfmpegFrameFeeder::open(const std::string_view path) {
    AVFormatContext *format_ctx{};
    if (ffmpeg->avformat_open_input(&format_ctx, path.data(), nullptr, nullptr) < 0)
        throw std::runtime_error("Could not open video file");
    format_ctx_guard.reset(format_ctx);

    // Find stream info
    if (ffmpeg->avformat_find_stream_info(format_ctx, nullptr) < 0)
        throw std::runtime_error("Could not find stream info");

    // Find video stream
    for (uint32_t i = 0; i < format_ctx->nb_streams; ++i) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_idx == -1)
        throw std::runtime_error("No video stream found");

    // Get codec parameters and decoder
    const auto *codec_parameters = format_ctx->streams[video_stream_idx]->codecpar;
    const auto *codec = ffmpeg->avcodec_find_decoder(codec_parameters->codec_id);
    if (!codec)
        throw std::runtime_error("Codec not found");

    auto *codec_ctx = ffmpeg->avcodec_alloc_context3(codec);
    if (!codec_ctx || ffmpeg->avcodec_parameters_to_context(codec_ctx, codec_parameters) < 0)
        throw std::runtime_error("Failed to set up codec context");
    codec_ctx_guard.reset(codec_ctx);

    if (ffmpeg->avcodec_open2(codec_ctx, codec, nullptr) < 0)
        throw std::runtime_error("Could not open codec");
}

FfmpegFrameFeeder::play_stats_t FfmpegFrameFeeder::play(benchmarking::MockTestConnection *connection) const {
    // Allocate frame and packet
    const FFmpeg::FrameGuard frame{ffmpeg->av_frame_alloc()};
    const FFmpeg::PacketGuard packet{ffmpeg->av_packet_alloc()};

    if (!frame || !packet)
        throw std::runtime_error("Could not allocate frame or packet");

    // Scaling context to convert to RGB24
    auto *sws_ctx = ffmpeg->sws_getContext(codec_ctx_guard->width,
                                           codec_ctx_guard->height,
                                           codec_ctx_guard->pix_fmt,
                                           codec_ctx_guard->width,
                                           codec_ctx_guard->height,
                                           AV_PIX_FMT_RGB24,
                                           SWS_BILINEAR,
                                           nullptr,
                                           nullptr,
                                           nullptr);
    if (!sws_ctx)
        throw std::runtime_error("Could not create scaling context");

    const std::unique_ptr<SwsContext, void (*)(SwsContext *)> sws_ctx_guard(sws_ctx, ffmpeg->sws_freeContext);

    const FFmpeg::FrameGuard rgb_frame{ffmpeg->av_frame_alloc()};
    if (!rgb_frame)
        throw std::runtime_error("Could not allocate frame");

    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width = codec_ctx_guard->width;
    rgb_frame->height = codec_ctx_guard->height;

    if (ffmpeg->av_frame_get_buffer(rgb_frame.get(), 0) != 0)
        throw std::runtime_error("Could not allocate frame data");

    play_stats_t stats{};
    const auto total_frame_count = get_total_frame_count();
    stats.timings.reserve(total_frame_count > 0 ? total_frame_count : 2048);

    auto *codec_ctx = codec_ctx_guard.get();

    while (ffmpeg->av_read_frame(format_ctx_guard.get(), packet.get()) == 0) {
        if (packet->stream_index == video_stream_idx) {

            if (ffmpeg->avcodec_send_packet(codec_ctx, packet.get()) == 0) {
                while (ffmpeg->avcodec_receive_frame(codec_ctx, frame.get()) == 0) {
                    // Convert to RGB
                    if (ffmpeg->sws_scale(sws_ctx_guard.get(),
                                          frame->data,
                                          frame->linesize,
                                          0,
                                          frame->height,
                                          rgb_frame->data,
                                          rgb_frame->linesize) < 0)
                        throw std::runtime_error("Could not scale frame");

                    connection->framebufferUpdateStart();
                    connection->setNewFrame(rgb_frame.get());
                    using namespace std::chrono;

                    auto now = high_resolution_clock::now();
                    connection->framebufferUpdateEnd();
                    const auto duration = duration_cast<milliseconds>(high_resolution_clock::now() - now).count();

                    // vlog.info("Frame took %lu ms", duration);
                    stats.total += duration;
                    stats.timings.push_back(duration);
                }
            }
        }
        ffmpeg->av_packet_unref(packet.get());
    }

    if (ffmpeg->av_seek_frame(format_ctx_guard.get(), video_stream_idx, 0, AVSEEK_FLAG_BACKWARD) < 0)
        throw std::runtime_error("Could not seek to start of video");

    ffmpeg->avcodec_flush_buffers(codec_ctx);

    return stats;
}

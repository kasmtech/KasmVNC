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

#include "ffmpeg.h"
#include <array>
#include <string_view>
#include <filesystem>

FFmpegFrameFeeder::FFmpegFrameFeeder() {
    static constexpr std::array<std::string_view, 2> paths = {
        "/usr/lib/",
        "/usr/lib64"
    };

    namespace fs = std::filesystem;
    using namespace std::string_literals;

    auto load_lib = [](auto *lib) {
        void *handle{};
        for (const auto &dir: paths) {
            if (!fs::exists(dir) || !fs::is_directory(dir))
                continue;

            for (const auto &entry: fs::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file())
                    continue;

                const std::string filename = entry.path().filename().string();
                if (filename.find(lib) != std::string::npos) {
                    handle = dlopen(filename.c_str(), RTLD_LAZY);

                    break;
                }
            }
        }

        if (!handle)
            throw std::runtime_error("Could not open "s + lib);

        return DlHandlerGuard{handle};
    };

    // libavformat
    libavformat = load_lib("libavformat.so");
    auto handle = libavformat.get();

    avformat_open_input_f = D_LOOKUP_SYM(handle, avformat_open_input);
    avformat_find_stream_info_f = D_LOOKUP_SYM(handle, avformat_find_stream_info);
    avcodec_find_decoder_f = D_LOOKUP_SYM(handle, avcodec_find_decoder);
    avcodec_parameters_to_context_f = D_LOOKUP_SYM(handle, avcodec_parameters_to_context);
    av_read_frame_f = D_LOOKUP_SYM(handle, av_read_frame);
    av_seek_frame_f = D_LOOKUP_SYM(handle, av_seek_frame);
    avformat_close_input_f = D_LOOKUP_SYM(handle, avformat_close_input);

    vlog.info("libavformat.so loaded");

    // libavutil
    libavutil = load_lib("libavutil.so");
    handle = libavutil.get();

    av_frame_free_f = D_LOOKUP_SYM(handle, av_frame_free);
    av_frame_alloc_f = D_LOOKUP_SYM(handle, av_frame_alloc);
    av_frame_get_buffer_f = D_LOOKUP_SYM(handle, av_frame_get_buffer);

    vlog.info("libavutil.so loaded");

    // libswscale
    libswscale = load_lib("libswscale.so");
    handle = libswscale.get();

    sws_freeContext_f = D_LOOKUP_SYM(handle, sws_freeContext);
    sws_getContext_f = D_LOOKUP_SYM(handle, sws_getContext);
    sws_scale_f = D_LOOKUP_SYM(handle, sws_scale);

    // libavcodec
    libavcodec = load_lib("libavcodec.so");
    handle = libavcodec.get();

    avcodec_open2_f = D_LOOKUP_SYM(handle, avcodec_open2);
    avcodec_alloc_context3_f = D_LOOKUP_SYM(handle, avcodec_alloc_context3);
    avcodec_send_packet_f = D_LOOKUP_SYM(handle, avcodec_send_packet);
    avcodec_receive_frame_f = D_LOOKUP_SYM(handle, avcodec_receive_frame);
    av_packet_unref_f = D_LOOKUP_SYM(handle, av_packet_unref);
    avcodec_flush_buffers_f = D_LOOKUP_SYM(handle, avcodec_flush_buffers);
    avcodec_close_f = D_LOOKUP_SYM(handle, avcodec_close);
    av_packet_alloc_f = D_LOOKUP_SYM(handle, av_packet_alloc);
    av_packet_free_f = D_LOOKUP_SYM(handle, av_packet_free);
}

FFmpegFrameFeeder::~FFmpegFrameFeeder() {
    avformat_close_input_f(&format_ctx);
    avcodec_close_f(codec_ctx);
    avcodec_free_context_f(&codec_ctx);
}

void FFmpegFrameFeeder::open(const std::string_view path) {
    if (avformat_open_input_f(&format_ctx, path.data(), nullptr, nullptr) < 0)
        throw std::runtime_error("Could not open video file");

    // Find stream info
    if (avformat_find_stream_info_f(format_ctx, nullptr) < 0)
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
    const auto *codec = avcodec_find_decoder_f(codec_parameters->codec_id);
    if (!codec)
        throw std::runtime_error("Codec not found");

    codec_ctx = avcodec_alloc_context3_f(codec);
    if (!codec_ctx || avcodec_parameters_to_context_f(codec_ctx, codec_parameters) < 0)
        throw std::runtime_error("Failed to set up codec context");

    if (avcodec_open2_f(codec_ctx, codec, nullptr) < 0)
        throw std::runtime_error("Could not open codec");
}

FFmpegFrameFeeder::play_stats_t FFmpegFrameFeeder::play(benchmarking::MockTestConnection *connection) const {
    // Allocate frame and packet
    const FrameGuard frame{av_frame_alloc_f()};
    const PacketGuard packet{av_packet_alloc_f()};

    if (!frame || !packet)
        throw std::runtime_error("Could not allocate frame or packet");

    // Scaling context to convert to RGB24
    SwsContext *sws_ctx = sws_getContext_f(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    if (!sws_ctx)
        throw std::runtime_error("Could not create scaling context");

    const std::unique_ptr<SwsContext, void(*)(SwsContext *)> sws_ctx_guard{sws_ctx, sws_freeContext_f};

    const FrameGuard rgb_frame{av_frame_alloc_f()};
    if (!rgb_frame)
        throw std::runtime_error("Could not allocate frame");

    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width = codec_ctx->width;
    rgb_frame->height = codec_ctx->height;

    if (av_frame_get_buffer_f(rgb_frame.get(), 0) != 0)
        throw std::runtime_error("Could not allocate frame data");

    play_stats_t stats{};
    const auto total_frame_count = get_total_frame_count();
    stats.timings.reserve(total_frame_count > 0 ? total_frame_count : 2048);

    while (av_read_frame_f(format_ctx, packet.get()) == 0) {
        if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet_f(codec_ctx, packet.get()) == 0) {
                while (avcodec_receive_frame_f(codec_ctx, frame.get()) == 0) {
                    // Convert to RGB
                    sws_scale_f(sws_ctx_guard.get(), frame->data, frame->linesize, 0,
                                frame->height,
                                rgb_frame->data, rgb_frame->linesize);

                    connection->framebufferUpdateStart();
                    connection->setNewFrame(rgb_frame.get());
                    using namespace std::chrono;

                    auto now = high_resolution_clock::now();
                    connection->framebufferUpdateEnd();
                    const auto duration = duration_cast<milliseconds>(high_resolution_clock::now() - now).count();

                    //vlog.info("Frame took %lu ms", duration);
                    stats.total += duration;
                    stats.timings.push_back(duration);
                }
            }
        }
        av_packet_unref_f(packet.get());
    }

    if (av_seek_frame_f(format_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD) < 0)
        throw std::runtime_error("Could not seek to start of video");

    avcodec_flush_buffers_f(codec_ctx);

    return stats;
}

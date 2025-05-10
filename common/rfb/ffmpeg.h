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

#pragma once

#include <dlfcn.h>
#include <memory>
#include <string>
#include "LogWriter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include "benchmark.h"

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define CONCAT_STR(a, b) a b

#define D_LOOKUP_SYM(handle, name) \
    [](auto handle, auto *sym_name) -> auto { \
        auto *sym = reinterpret_cast<name##_func>(dlsym(handle, sym_name)); \
        if (!sym) \
            throw std::runtime_error("Failed to load symbol "s + sym_name); \
        return sym; \
    }(handle, STR(name))

#define DEFINE_GUARD(name, type, deleter) \
    using name##Guard = std::unique_ptr<type, decltype([](auto *ptr){deleter##_f(&ptr);})>;

//using SwsContextGuard = std::unique_ptr<SwsContext, SwsContextDeleter>;

class FFmpegFrameFeeder final {
    // libavformat
    using avformat_close_input_func = void(*)(AVFormatContext **);
    using avformat_open_input_func = int(*)(AVFormatContext **ps, const char *url, const AVInputFormat *fmt,
                                            AVDictionary **options);
    using avformat_find_stream_info_func = int (*)(AVFormatContext *ic, AVDictionary **options);
    using av_read_frame_func = int (*)(AVFormatContext *s, AVPacket *pkt);
    using av_seek_frame_func = int (*)(AVFormatContext *s, int stream_index, int64_t timestamp, int flags);

    // libavutil
    using av_frame_free_func = void (*)(AVFrame **);
    using av_frame_alloc_func = AVFrame *(*)();
    using av_frame_get_buffer_func = int (*)(AVFrame *frame, int align);

    // libswscale
    using sws_freeContext_func = void (*)(SwsContext *);
    using sws_getContext_func = SwsContext * (*)(int srcW, int srcH, AVPixelFormat srcFormat, int dstW, int dstH,
                                                 AVPixelFormat dstFormat, int flags, SwsFilter *srcFilter,
                                                 SwsFilter *dstFilter, const double *param);

    using sws_scale_func = int(*)(SwsContext *c, const uint8_t *const srcSlice[], const int srcStride[], int srcSliceY,
                                  int srcSliceH, uint8_t *const dst[], const int dstStride[]);

    // libavcodec
    using avcodec_free_context_func = void (*)(AVCodecContext **);
    using av_packet_free_func = void (*)(AVPacket **);
    using avcodec_find_decoder_func = const AVCodec * (*)(AVCodecID id);
    using avcodec_alloc_context3_func = AVCodecContext* (*)(const AVCodec *codec);
    using avcodec_parameters_to_context_func = int (*)(AVCodecContext *codec, const AVCodecParameters *par);
    using avcodec_open2_func = int (*)(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);
    using av_packet_alloc_func = AVPacket *(*)();
    using avcodec_send_packet_func = int(*)(AVCodecContext *avctx, const AVPacket *avpkt);
    using avcodec_receive_frame_func = int(*)(AVCodecContext *avctx, AVFrame *frame);
    using av_packet_unref_func = void (*)(AVPacket *pkt);
    using avcodec_flush_buffers_func = void (*)(AVCodecContext *avctx);
    using avcodec_close_func = int (*)(AVCodecContext *avctx);

    struct DlHandler {
        void operator()(void *handle) const {
            dlclose(handle);
        }
    };

    using DlHandlerGuard = std::unique_ptr<void, DlHandler>;

    // libavformat
    avformat_close_input_func avformat_close_input_f{};
    avformat_open_input_func avformat_open_input_f{};
    avformat_find_stream_info_func avformat_find_stream_info_f{};
    av_read_frame_func av_read_frame_f{};
    av_seek_frame_func av_seek_frame_f{};

    // libavutil
    static inline av_frame_free_func av_frame_free_f{};
    av_frame_alloc_func av_frame_alloc_f{};
    av_frame_get_buffer_func av_frame_get_buffer_f{};

    // libswscale
    sws_freeContext_func sws_freeContext_f{};
    sws_getContext_func sws_getContext_f{};
    sws_scale_func sws_scale_f{};

    // libavcodec
    avcodec_free_context_func avcodec_free_context_f{};
    static inline av_packet_free_func av_packet_free_f{};
    avcodec_find_decoder_func avcodec_find_decoder_f{};
    avcodec_alloc_context3_func avcodec_alloc_context3_f{};
    avcodec_parameters_to_context_func avcodec_parameters_to_context_f{};
    avcodec_open2_func avcodec_open2_f{};
    av_packet_alloc_func av_packet_alloc_f{};
    avcodec_send_packet_func avcodec_send_packet_f{};
    avcodec_receive_frame_func avcodec_receive_frame_f{};
    av_packet_unref_func av_packet_unref_f{};
    avcodec_flush_buffers_func avcodec_flush_buffers_f{};
    avcodec_close_func avcodec_close_f{};

    rfb::LogWriter vlog{"FFmpeg"};

    DEFINE_GUARD(Frame, AVFrame, av_frame_free)
    DEFINE_GUARD(Packet, AVPacket, av_packet_free)

    AVFormatContext *format_ctx{};
    AVCodecContext *codec_ctx{};
    int video_stream_idx{-1};

    DlHandlerGuard libavformat{};
    DlHandlerGuard libavutil{};
    DlHandlerGuard libswscale{};
    DlHandlerGuard libavcodec{};

public:
    FFmpegFrameFeeder();

    ~FFmpegFrameFeeder();

    void open(std::string_view path);

    [[nodiscard]] int64_t get_total_frame_count() const {
        return format_ctx->streams[video_stream_idx]->nb_frames;
    }

    struct frame_dimensions_t {
        int width{};
        int height{};
    };

    [[nodiscard]] frame_dimensions_t get_frame_dimensions() const {
        return {codec_ctx->width, codec_ctx->height};
    }

    struct play_stats_t {
        uint64_t frames{};
        uint64_t total{};
        std::vector<uint64_t> timings;
    };

    play_stats_t play(benchmarking::MockTestConnection *connection) const;
};

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
#include <filesystem>
#include "LogWriter.h"

static rfb::LogWriter vlog("ffmpeg");

FFmpeg::FFmpeg() {

    static constexpr std::array<std::string_view, 2> paths = {"/usr/lib/", "/usr/lib64"};

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
    try {
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
        av_frame_unref_f = D_LOOKUP_SYM(handle, av_frame_unref);
        av_frame_get_buffer_f = D_LOOKUP_SYM(handle, av_frame_get_buffer);
        av_opt_next_f = D_LOOKUP_SYM(handle, av_opt_next);
        av_opt_set_f = D_LOOKUP_SYM(handle, av_opt_set);
        av_opt_set_int_f = D_LOOKUP_SYM(handle, av_opt_set_int);
        av_buffer_unref_f = D_LOOKUP_SYM(handle, av_buffer_unref);
        av_hwdevice_ctx_create_f = D_LOOKUP_SYM(handle, av_hwdevice_ctx_create);
        av_hwframe_ctx_alloc_f = D_LOOKUP_SYM(handle, av_hwframe_ctx_alloc);
        av_hwframe_ctx_init_f = D_LOOKUP_SYM(handle, av_hwframe_ctx_init);
        av_buffer_ref_f = D_LOOKUP_SYM(handle, av_buffer_ref);
        av_hwframe_get_buffer_f = D_LOOKUP_SYM(handle, av_hwframe_get_buffer);
        av_hwframe_transfer_data_f = D_LOOKUP_SYM(handle, av_hwframe_transfer_data);
        av_strerror_f = D_LOOKUP_SYM(handle, av_strerror);
        av_log_set_level_f = D_LOOKUP_SYM(handle, av_log_set_level);
        av_log_set_callback_f = D_LOOKUP_SYM(handle, av_log_set_callback);

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

        avcodec_free_context_f = D_LOOKUP_SYM(handle, avcodec_free_context);
        avcodec_open2_f = D_LOOKUP_SYM(handle, avcodec_open2);
        avcodec_find_encoder_f = D_LOOKUP_SYM(handle, avcodec_find_encoder);
        avcodec_find_encoder_by_name_f = D_LOOKUP_SYM(handle, avcodec_find_encoder_by_name);
        avcodec_alloc_context3_f = D_LOOKUP_SYM(handle, avcodec_alloc_context3);
        avcodec_send_frame_f = D_LOOKUP_SYM(handle, avcodec_send_frame);
        avcodec_send_packet_f = D_LOOKUP_SYM(handle, avcodec_send_packet);
        avcodec_receive_frame_f = D_LOOKUP_SYM(handle, avcodec_receive_frame);
        avcodec_receive_packet_f = D_LOOKUP_SYM(handle, avcodec_receive_packet);
        av_packet_unref_f = D_LOOKUP_SYM(handle, av_packet_unref);
        avcodec_flush_buffers_f = D_LOOKUP_SYM(handle, avcodec_flush_buffers);
        av_codec_is_encoder_f = D_LOOKUP_SYM(handle, av_codec_is_encoder);
        av_packet_alloc_f = D_LOOKUP_SYM(handle, av_packet_alloc);
        av_packet_free_f = D_LOOKUP_SYM(handle, av_packet_free);

        av_log_set_level_f(AV_LOG_VERBOSE); // control what is emitted
        av_log_set_callback_f(av_log_callback);

        available = true;
    } catch (std::exception &e) {
        vlog.error("%s", e.what());

        return;
    }
}

void FFmpeg::av_log_callback(void *ptr, int level, const char *fmt, va_list vl) {
    if (level > AV_LOG_VERBOSE)
        return;

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, vl);
    vlog.debug("[FFmpeg Debug] %s", buffer);
}

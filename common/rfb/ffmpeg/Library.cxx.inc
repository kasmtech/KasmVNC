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

#include "Library.h"

#include <array>
#include <filesystem>
#include "rfb/LogWriter.h"

namespace KASMVNC_FFMPEG {
static rfb::LogWriter vlog("ffmpeg-library");
using namespace std::string_literals;

#ifndef KASMVNC_STATIC_FFMPEG
Library::DlHandlerGuard Library::loadLibrary(const char *lib, unsigned major_version) {
    const auto soname = fmt::format("{}.{}", lib, major_version);
    auto *handle = dlopen(soname.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
    if (!handle)
        throw std::runtime_error("Could not open " + soname + ": " + dlerror());
    return DlHandlerGuard{handle};
}

#endif

Library::Library() {
    try {
#ifndef KASMVNC_STATIC_FFMPEG
        // Load the codec first: its DT_NEEDED dependencies select the matching util.
        libavcodec = loadLibrary("libavcodec.so", LIBAVCODEC_VERSION_MAJOR);
        auto *codec_handle = libavcodec.get();
        auto codec_version = reinterpret_cast<unsigned (*)()>(dlsym(codec_handle, "avcodec_version"));
        util_version_symbol = dlsym(codec_handle, "avutil_version");
        auto util_version = reinterpret_cast<unsigned (*)()>(util_version_symbol);
        if (!codec_version || !util_version ||
            AV_VERSION_MAJOR(codec_version()) != LIBAVCODEC_VERSION_MAJOR ||
            AV_VERSION_MAJOR(util_version()) != LIBAVUTIL_VERSION_MAJOR ||
            AV_VERSION_MINOR(codec_version()) < LIBAVCODEC_VERSION_MINOR ||
            AV_VERSION_MINOR(util_version()) < LIBAVUTIL_VERSION_MINOR)
            throw std::runtime_error("Inconsistent FFmpeg codec/util library family");
        void *handle{};
        // libavutil
        handle = codec_handle; // Resolve util from the selected codec dependency scope.
#endif

        av_frame_free_f = D_LOOKUP_SYM(handle, av_frame_free);
        av_frame_alloc_f = D_LOOKUP_SYM(handle, av_frame_alloc);
        av_frame_make_writable_f = D_LOOKUP_SYM(handle, av_frame_make_writable);
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

        vlog.debug("libavutil API initialized");

        // libavcodec
#ifndef KASMVNC_STATIC_FFMPEG
        handle = libavcodec.get();
#endif

        avcodec_find_decoder_f = D_LOOKUP_SYM(handle, avcodec_find_decoder);
        avcodec_parameters_to_context_f = D_LOOKUP_SYM(handle, avcodec_parameters_to_context);
        avcodec_version_f = D_LOOKUP_SYM(handle, avcodec_version);
        avcodec_configuration_f = D_LOOKUP_SYM(handle, avcodec_configuration);
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
        vlog.debug("libavcodec API initialized");

        av_log_set_level_f(AV_LOG_VERBOSE); // control what is emitted
        av_log_set_callback_f(av_log_callback);

        const auto ver = avcodec_version_f();
        vlog.debug("libavcodec version: %d.%d.%d", AV_VERSION_MAJOR(ver), AV_VERSION_MINOR(ver), AV_VERSION_MICRO(ver));

        const auto *config = avcodec_configuration_f();
        vlog.debug("libavcodec configuration: %s", config);

#ifdef KASMVNC_STATIC_FFMPEG
        vlog.info("Library: static linkage");
#endif
        available = true;
    } catch (std::exception &e) {
        vlog.debug("FFmpeg %d runtime not usable: %s", LIBAVCODEC_VERSION_MAJOR, e.what());

        return;
    }
}

// File-backed benchmarks need demuxing, but normal sessions do not. Defer
// libavformat and its dependencies until a caller actually uses that API.
void Library::ensureFormat() {
    // Library is a singleton; local-static initialization is thread-safe and
    // retries on the next call if loading throws.
    [[maybe_unused]] static const bool initialized = [this] {
        // libswscale
#ifndef KASMVNC_STATIC_FFMPEG
        libswscale = loadLibrary("libswscale.so", LIBSWSCALE_VERSION_MAJOR);
        auto *handle = libswscale.get();
        auto scale_version = reinterpret_cast<unsigned (*)()>(dlsym(handle, "swscale_version"));
        if (!scale_version || AV_VERSION_MAJOR(scale_version()) != LIBSWSCALE_VERSION_MAJOR ||
            AV_VERSION_MINOR(scale_version()) < LIBSWSCALE_VERSION_MINOR ||
            dlsym(handle, "avutil_version") != util_version_symbol)
            throw std::runtime_error("Inconsistent FFmpeg swscale/util library family");
#endif

        sws_freeContext_f = D_LOOKUP_SYM(handle, sws_freeContext);
        sws_getContext_f = D_LOOKUP_SYM(handle, sws_getContext);
        sws_scale_f = D_LOOKUP_SYM(handle, sws_scale);
        vlog.debug("libswscale API initialized");


#ifndef KASMVNC_STATIC_FFMPEG
        auto library = loadLibrary("libavformat.so", LIBAVFORMAT_VERSION_MAJOR);
        handle = library.get();
        auto format_version = reinterpret_cast<unsigned (*)()>(dlsym(handle, "avformat_version"));
        if (!format_version || AV_VERSION_MAJOR(format_version()) != LIBAVFORMAT_VERSION_MAJOR ||
            AV_VERSION_MINOR(format_version()) < LIBAVFORMAT_VERSION_MINOR ||
            dlsym(handle, "avutil_version") != util_version_symbol ||
            dlsym(handle, "avcodec_version") != reinterpret_cast<void *>(avcodec_version_f))
            throw std::runtime_error("Inconsistent FFmpeg format/codec/util library family");
#endif
        avformat_open_input_f = D_LOOKUP_SYM(handle, avformat_open_input);
        avformat_find_stream_info_f = D_LOOKUP_SYM(handle, avformat_find_stream_info);
        av_read_frame_f = D_LOOKUP_SYM(handle, av_read_frame);
        av_seek_frame_f = D_LOOKUP_SYM(handle, av_seek_frame);
        avformat_close_input_f = D_LOOKUP_SYM(handle, avformat_close_input);
#ifndef KASMVNC_STATIC_FFMPEG
        libavformat = std::move(library);
        vlog.debug("libavformat.so loaded on demand");
#else
        vlog.debug("Statically linked libavformat initialized on demand");
#endif
        return true;
    }();
}

void Library::av_log_callback(void *ptr, int level, const char *fmt, va_list vl) {
    if (level > AV_LOG_VERBOSE)
        return;

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, vl);
    vlog.debug("[FFmpeg Debug] %s", buffer);
}

std::string Library::description() const {
#ifdef KASMVNC_STATIC_FFMPEG
    return "static FFmpeg " + std::to_string(LIBAVCODEC_VERSION_MAJOR);
#else
    Dl_info info{};
    dladdr(reinterpret_cast<void *>(avcodec_version_f), &info);
    return "FFmpeg avcodec " + std::to_string(LIBAVCODEC_VERSION_MAJOR) + " from " +
           (info.dli_fname ? info.dli_fname : "unknown path");
#endif
}
} // namespace KASMVNC_FFMPEG

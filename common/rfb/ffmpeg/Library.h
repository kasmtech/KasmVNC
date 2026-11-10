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
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define CONCAT_STR(a, b) a b

// CMake selects direct symbol references only for the static backend target.
#ifdef KASMVNC_STATIC_FFMPEG
#define D_LOOKUP_SYM(handle, name) (&::name)
#else
#define D_LOOKUP_SYM(handle, name)                                                                                     \
    [](auto handle, auto *sym_name) -> auto {                                                                          \
        auto *sym = reinterpret_cast<name##_func>(dlsym(handle, sym_name));                                            \
        if (!sym)                                                                                                      \
            throw std::runtime_error("Failed to load symbol "s + sym_name);                                            \
        return sym;                                                                                                    \
    }(handle, STR(name))
#endif


// using SwsContextGuard = std::unique_ptr<SwsContext, SwsContextDeleter>;

namespace KASMVNC_FFMPEG {

class Library final {
    struct AVFrameDeleter {
        void operator()(AVFrame *frame) const {
            av_frame_free(&frame);
        }
    };

    struct AVPacketDeleter {
        void operator()(AVPacket *pkt) const {
            av_packet_free(&pkt);
        }
    };

    struct AVCodecContextDeleter {
        void operator()(AVCodecContext *ctx) const {
            avcodec_free_context(&ctx);
        }
    };

    struct AVFormatContextDeleter {
        void operator()(AVFormatContext *ctx) const {
            avformat_close_input(&ctx);
        }
    };

    struct SwsContextDeleter {
        void operator()(SwsContext *ctx) const {
            sws_freeContext(ctx);
        }
    };

    struct AVBufferRefDeleter {
        void operator()(AVBufferRef *buf) const {
            av_buffer_unref(&buf);
        }
    };

#define DEFINE_GUARD(name, type) using name##Guard = std::unique_ptr<type, type##Deleter>;

    // Derive exact function signatures from this adapter's headers. Older
    // families differ in constness (codec lookup, input formats, buffer refs).
    // These types never cross the Kasm interface or another adapter.
    // libavformat
    using avformat_close_input_func = decltype(&::avformat_close_input);
    using avformat_open_input_func = decltype(&::avformat_open_input);
    using avformat_find_stream_info_func = decltype(&::avformat_find_stream_info);
    using av_read_frame_func = decltype(&::av_read_frame);
    using av_seek_frame_func = decltype(&::av_seek_frame);

    // libavutil
    using av_frame_free_func = decltype(&::av_frame_free);
    using av_frame_alloc_func = decltype(&::av_frame_alloc);
    using av_frame_get_buffer_func = decltype(&::av_frame_get_buffer);
    using av_frame_make_writable_func = decltype(&::av_frame_make_writable);
    using av_frame_unref_func = decltype(&::av_frame_unref);
    using av_opt_next_func = decltype(&::av_opt_next);
    using av_opt_set_func = decltype(&::av_opt_set);
    using av_opt_set_int_func = decltype(&::av_opt_set_int);
    using av_buffer_unref_func = decltype(&::av_buffer_unref);
    using av_hwdevice_ctx_create_func = decltype(&::av_hwdevice_ctx_create);
    using av_hwframe_ctx_alloc_func = decltype(&::av_hwframe_ctx_alloc);
    using av_hwframe_ctx_init_func = decltype(&::av_hwframe_ctx_init);
    using av_buffer_ref_func = decltype(&::av_buffer_ref);
    using av_hwframe_get_buffer_func = decltype(&::av_hwframe_get_buffer);
    using av_hwframe_transfer_data_func = decltype(&::av_hwframe_transfer_data);
    using av_strerror_func = decltype(&::av_strerror);
    using av_log_set_level_func = decltype(&::av_log_set_level);
    using av_log_set_callback_func = decltype(&::av_log_set_callback);

    // libswscale
    using sws_freeContext_func = decltype(&::sws_freeContext);
    using sws_getContext_func = decltype(&::sws_getContext);

    using sws_scale_func = decltype(&::sws_scale);

    // libavcodec
    using avcodec_version_func = decltype(&::avcodec_version);
    using avcodec_configuration_func = decltype(&::avcodec_configuration);
    using avcodec_free_context_func = decltype(&::avcodec_free_context);
    using av_packet_free_func = decltype(&::av_packet_free);
    using avcodec_find_encoder_func = decltype(&::avcodec_find_encoder);
    using avcodec_find_encoder_by_name_func = decltype(&::avcodec_find_encoder_by_name);
    using avcodec_find_decoder_func = decltype(&::avcodec_find_decoder);
    using avcodec_alloc_context3_func = decltype(&::avcodec_alloc_context3);
    using avcodec_parameters_to_context_func = decltype(&::avcodec_parameters_to_context);
    using avcodec_open2_func = decltype(&::avcodec_open2);
    using av_packet_alloc_func = decltype(&::av_packet_alloc);
    using avcodec_send_frame_func = decltype(&::avcodec_send_frame);
    using avcodec_send_packet_func = decltype(&::avcodec_send_packet);
    using avcodec_receive_packet_func = decltype(&::avcodec_receive_packet);
    using avcodec_receive_frame_func = decltype(&::avcodec_receive_frame);
    using av_packet_unref_func = decltype(&::av_packet_unref);
    using avcodec_flush_buffers_func = decltype(&::avcodec_flush_buffers);
    using av_codec_is_encoder_func = decltype(&::av_codec_is_encoder);

    // libavfilter
    /*using avfilter_graph_parse_ptr_func = int (*)(AVFilterGraph *graph, const char *filters, AVFilterInOut **inputs,
        AVFilterInOut **outputs, void *log_ctx);*/

    struct DlHandler {
        void operator()(void *handle) const {
            dlclose(handle);
        }
    };

    using DlHandlerGuard = std::unique_ptr<void, DlHandler>;

    // libavformat
    static inline avformat_close_input_func avformat_close_input_f{};
    avformat_open_input_func avformat_open_input_f{};
    avformat_find_stream_info_func avformat_find_stream_info_f{};
    av_read_frame_func av_read_frame_f{};
    av_seek_frame_func av_seek_frame_f{};

    // libavutil
    static inline av_frame_free_func av_frame_free_f{};
    av_frame_alloc_func av_frame_alloc_f{};
    av_frame_get_buffer_func av_frame_get_buffer_f{};
    av_frame_unref_func av_frame_unref_f{};
    av_frame_make_writable_func av_frame_make_writable_f{};
    av_opt_next_func av_opt_next_f{};
    av_opt_set_func av_opt_set_f{};
    av_opt_set_int_func av_opt_set_int_f{};
    static inline av_buffer_unref_func av_buffer_unref_f{};
    av_hwdevice_ctx_create_func av_hwdevice_ctx_create_f{};
    av_hwframe_ctx_alloc_func av_hwframe_ctx_alloc_f{};
    av_hwframe_ctx_init_func av_hwframe_ctx_init_f{};
    av_buffer_ref_func av_buffer_ref_f{};
    av_hwframe_get_buffer_func av_hwframe_get_buffer_f{};
    av_hwframe_transfer_data_func av_hwframe_transfer_data_f{};
    av_strerror_func av_strerror_f{};
    av_log_set_level_func av_log_set_level_f{};
    av_log_set_callback_func av_log_set_callback_f{};

    // libswscale
    static inline sws_freeContext_func sws_freeContext_f{};
    sws_getContext_func sws_getContext_f{};
    sws_scale_func sws_scale_f{};

    // libavcodec
    static inline avcodec_version_func avcodec_version_f{};
    static inline avcodec_configuration_func avcodec_configuration_f{};
    static inline avcodec_free_context_func avcodec_free_context_f{};
    static inline av_packet_free_func av_packet_free_f{};
    avcodec_find_encoder_func avcodec_find_encoder_f{};
    avcodec_find_encoder_by_name_func avcodec_find_encoder_by_name_f{};
    avcodec_find_decoder_func avcodec_find_decoder_f{};
    avcodec_alloc_context3_func avcodec_alloc_context3_f{};
    avcodec_parameters_to_context_func avcodec_parameters_to_context_f{};
    avcodec_open2_func avcodec_open2_f{};
    av_packet_alloc_func av_packet_alloc_f{};
    avcodec_send_frame_func avcodec_send_frame_f{};
    avcodec_send_packet_func avcodec_send_packet_f{};
    avcodec_receive_frame_func avcodec_receive_frame_f{};
    avcodec_receive_packet_func avcodec_receive_packet_f{};
    av_packet_unref_func av_packet_unref_f{};
    avcodec_flush_buffers_func avcodec_flush_buffers_f{};
    av_codec_is_encoder_func av_codec_is_encoder_f{};

    // libavfilter
    // avfilter_graph_parse_ptr_func avfilter_graph_parse_ptr_f{};

    void *util_version_symbol{};
    DlHandlerGuard libavformat{};
    DlHandlerGuard libavutil{};
    DlHandlerGuard libswscale{};
    DlHandlerGuard libavcodec{};
    DlHandlerGuard libavfilter{};

    static DlHandlerGuard loadLibrary(const char *lib, unsigned major_version);
    void ensureFormat();

    Library();
    ~Library() = default;

    static void av_log_callback(void *ptr, int level, const char *fmt, va_list vl);

    bool available{};

public:
    unsigned version() const { return avcodec_version_f(); }
    std::string description() const;
    [[nodiscard]] static Library &get() {
        static Library instance;
        return instance;
    }

    [[nodiscard]] bool is_available() const {
        return available;
    }

    static void avformat_close_input(AVFormatContext **s) {
        get().ensureFormat();
        avformat_close_input_f(s);
    }

    [[nodiscard]] int avformat_open_input(AVFormatContext **ps, const char *url, AVInputFormat *fmt,
                                          AVDictionary **options) const {
        get().ensureFormat();
        return avformat_open_input_f(ps, url, fmt, options);
    }

    [[nodiscard]] int avformat_find_stream_info(AVFormatContext *ic, AVDictionary **options) const {
        get().ensureFormat();
        return avformat_find_stream_info_f(ic, options);
    }

    [[nodiscard]] int av_read_frame(AVFormatContext *s, AVPacket *pkt) const {
        get().ensureFormat();
        return av_read_frame_f(s, pkt);
    }

    [[nodiscard]] int av_seek_frame(AVFormatContext *s, int stream_index, int64_t timestamp, int flags) const {
        get().ensureFormat();
        return av_seek_frame_f(s, stream_index, timestamp, flags);
    }

    // libavutil
    static void av_frame_free(AVFrame **frame) {
        av_frame_free_f(frame);
    }

    [[nodiscard]] AVFrame *av_frame_alloc() const {
        return av_frame_alloc_f();
    }

    [[nodiscard]] int av_frame_get_buffer(AVFrame *frame, int align) const {
        return av_frame_get_buffer_f(frame, align);
    }

    int av_frame_make_writable(AVFrame *frame) const { return av_frame_make_writable_f(frame); }

    void av_frame_unref(AVFrame *frame) const {
        av_frame_unref_f(frame);
    }

    [[nodiscard]] const AVOption *av_opt_next(const void *obj, const AVOption *prev) {
        return av_opt_next_f(obj, prev);
    }

    [[nodiscard]] int av_opt_set(void *obj, const char *name, const char *val, int search_flags) const {
        return av_opt_set_f(obj, name, val, search_flags);
    }

    [[nodiscard]] int av_opt_set_int(void *obj, const char *name, int64_t val, int search_flags) const {
        return av_opt_set_int_f(obj, name, val, search_flags);
    }

    static void av_buffer_unref(AVBufferRef **buf) {
        av_buffer_unref_f(buf);
    }

    [[nodiscard]] int av_hwdevice_ctx_create(AVBufferRef **device_ctx, AVHWDeviceType type, const char *device,
                                             AVDictionary *opts, int flags) const {
        return av_hwdevice_ctx_create_f(device_ctx, type, device, opts, flags);
    }

    [[nodiscard]] AVBufferRef *av_hwframe_ctx_alloc(AVBufferRef *device_ctx) const {
        return av_hwframe_ctx_alloc_f(device_ctx);
    }

    [[nodiscard]] int av_hwframe_ctx_init(AVBufferRef *ref) const {
        return av_hwframe_ctx_init_f(ref);
    }

    [[nodiscard]] AVBufferRef *av_buffer_ref(AVBufferRef *buf) const {
        return av_buffer_ref_f(buf);
    }

    [[nodiscard]] int av_hwframe_get_buffer(AVBufferRef *hwframe_ctx, AVFrame *frame, int flags) const {
        return av_hwframe_get_buffer_f(hwframe_ctx, frame, flags);
    }

    [[nodiscard]] int av_hwframe_transfer_data(AVFrame *dst, const AVFrame *src, int flags) const {
        return av_hwframe_transfer_data_f(dst, src, flags);
    }

    // libswscale
    static void sws_freeContext(SwsContext *sws_context) {
        sws_freeContext_f(sws_context);
    }

    [[nodiscard]] SwsContext *sws_getContext(int srcW, int srcH, AVPixelFormat srcFormat, int dstW, int dstH,
                                             AVPixelFormat dstFormat, int flags, SwsFilter *srcFilter,
                                             SwsFilter *dstFilter, const double *param) const {
        return sws_getContext_f(srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags, srcFilter, dstFilter, param);
    }

    [[nodiscard]] int sws_scale(SwsContext *c, const uint8_t *const src_slice[], const int src_stride[],
                                int src_slice_y, int src_slice_h, uint8_t *const dst[], const int dst_stride[]) const {
        return sws_scale_f(c, src_slice, src_stride, src_slice_y, src_slice_h, dst, dst_stride);
    };

    // libavcodec
    [[nodiscard]] const AVCodec *avcodec_find_encoder(AVCodecID id) const {
        return avcodec_find_encoder_f(id);
    }

    [[nodiscard]] const AVCodec *avcodec_find_decoder(AVCodecID id) const {
        return avcodec_find_decoder_f(id);
    }

    [[nodiscard]] int avcodec_parameters_to_context(AVCodecContext *codec, const AVCodecParameters *par) const {
        return avcodec_parameters_to_context_f(codec, par);
    }

    static void avcodec_free_context(AVCodecContext **avctx) {
        avcodec_free_context_f(avctx);
    }

    static void av_packet_free(AVPacket **pkt) {
        av_packet_free_f(pkt);
    }

    [[nodiscard]] AVCodecContext *avcodec_alloc_context3(const AVCodec *codec) const {
        return avcodec_alloc_context3_f(codec);
    }

    [[nodiscard]] int avcodec_open2(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options) const {
        return avcodec_open2_f(avctx, codec, options);
    }

    [[nodiscard]] const AVCodec *avcodec_find_encoder_by_name(const char *name) const {
        return avcodec_find_encoder_by_name_f(name);
    }

    [[nodiscard]] AVPacket *av_packet_alloc() const {
        return av_packet_alloc_f();
    }

    int avcodec_send_frame(AVCodecContext *avctx, const AVFrame *frame) const {
        return avcodec_send_frame_f(avctx, frame);
    }

    [[nodiscard]] int avcodec_send_packet(AVCodecContext *avctx, const AVPacket *avpkt) const {
        return avcodec_send_packet_f(avctx, avpkt);
    }

    [[nodiscard]] int avcodec_receive_frame(AVCodecContext *avctx, AVFrame *frame) const {
        return avcodec_receive_frame_f(avctx, frame);
    }

    [[nodiscard]] int avcodec_receive_packet(AVCodecContext *avctx, AVPacket *avpkt) const {
        return avcodec_receive_packet_f(avctx, avpkt);
    }

    void av_packet_unref(AVPacket *pkt) const {
        av_packet_unref_f(pkt);
    }

    void avcodec_flush_buffers(AVCodecContext *avctx) const {
        avcodec_flush_buffers_f(avctx);
    }

    int av_codec_is_encoder(const AVCodec *codec) const {
        return av_codec_is_encoder_f(codec);
    }

    DEFINE_GUARD(Frame, AVFrame)
    DEFINE_GUARD(Packet, AVPacket)
    DEFINE_GUARD(Context, AVCodecContext)
    DEFINE_GUARD(FormatCtx, AVFormatContext)
    DEFINE_GUARD(SwsContext, SwsContext)
    DEFINE_GUARD(Buffer, AVBufferRef);

    [[nodiscard]] std::string get_error_description(int err) const {
        char errbuf[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror_f(err, errbuf, AV_ERROR_MAX_STRING_SIZE);

        return {errbuf};
    }
};

} // namespace KASMVNC_FFMPEG

/* Copyright (C) 2026 Kasm Technologies Corp. GPL-2.0-or-later. */
#include <config.h>
#include "Backend.h"
#include <algorithm>
#include <deque>
#include <stdexcept>
#include <libyuv.h>
// The symbol table and every use of FFmpeg structures belong to this versioned
// translation unit. The public interface uses only Kasm-owned types.
#include "Library.cxx.inc"

#if KASMVNC_EXPECTED_AVCODEC
static_assert(LIBAVCODEC_VERSION_MAJOR == KASMVNC_EXPECTED_AVCODEC,
              "FFmpeg adapter compiled with the wrong header family");
#endif

namespace KASMVNC_FFMPEG_NAMESPACE {
static void check(int result, const char *operation) {
    if (result < 0)
        throw std::runtime_error(std::string(operation) + ": " + Library::get().get_error_description(result));
}

class Encoder final : public FFmpeg::Encoder {
    Library &lib = Library::get();
    FFmpeg::EncoderSettings settings;
    Library::BufferGuard device;
    Library::BufferGuard pool;
    Library::ContextGuard context;
    Library::FrameGuard frame;
    Library::FrameGuard hw_frame;
    std::deque<Library::PacketGuard> packets;

    void option(const char *name, const char *value) {
        check(lib.av_opt_set(context->priv_data, name, value, 0), name);
    }
    void option(const char *name, int value) {
        check(lib.av_opt_set_int(context->priv_data, name, value, 0), name);
    }
    void receive() {
        for (;;) {
            Library::PacketGuard packet{lib.av_packet_alloc()};
            if (!packet) throw std::bad_alloc();
            const int result = lib.avcodec_receive_packet(context.get(), packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
            check(result, "receive encoded packet");
            // Low-delay video should not accumulate output indefinitely if a
            // caller stops consuming. Keep memory usage bounded on error paths.
            if (packets.size() >= 32) throw std::runtime_error("Encoded packet queue was not consumed");
            packets.push_back(std::move(packet));
        }
    }
public:
    explicit Encoder(const FFmpeg::EncoderSettings &value) : settings(value) {
        if (settings.width < 2 || settings.height < 2 || settings.width % 2 || settings.height % 2 ||
            settings.frame_rate <= 0 || settings.gop < 0)
            throw std::invalid_argument("Invalid encoder dimensions or frame timing");
        const auto *codec = lib.avcodec_find_encoder_by_name(settings.name.c_str());
        if (!codec || codec->type != AVMEDIA_TYPE_VIDEO || !lib.av_codec_is_encoder(codec))
            throw std::runtime_error("Encoder is not available: " + settings.name);
        context.reset(lib.avcodec_alloc_context3(codec));
        frame.reset(lib.av_frame_alloc());
        if (!context || !frame) throw std::bad_alloc();
        context->time_base = {1, settings.frame_rate};
        context->framerate = {settings.frame_rate, 1};
        context->gop_size = settings.gop;
        context->max_b_frames = 0;
        context->width = settings.width;
        context->height = settings.height;
        context->profile = settings.profile;
        const bool hardware = settings.device != FFmpeg::Device::Software;
        context->pix_fmt = hardware ? (settings.device == FFmpeg::Device::CUDA ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI)
                                   : AV_PIX_FMT_YUV420P;
        if (!hardware) {
            if (settings.name == "libsvtav1") {
                option("preset", "12");
                option("svtav1-params", "rtc=1");
            } else {
                option("tune", "zerolatency");
                option("preset", "ultrafast");
            }
            option("crf", settings.quality);
        } else {
            context->flags |= AV_CODEC_FLAG_LOW_DELAY;
            context->delay = 0;
            if (settings.device == FFmpeg::Device::CUDA) {
                context->refs = 0;
                option("rc", "vbr");
                option("preset", "p4");
                option("tune", "ull");
                option("zerolatency", 1);
                option("b_ref_mode", 0);
                option("delay", "0");
                // qmin/qmax are generic codec options, not private NVENC options.
                context->qmin = settings.quality;
                context->qmax = settings.maximum_quality;
                option("cq", settings.quality);
            } else {
                option("async_depth", "1");
                option("rc_mode", "CQP");
                option("qp", settings.quality);
            }
            AVBufferRef *raw_device{};
            const char *path = settings.device_path.empty() ? nullptr : settings.device_path.c_str();
            check(lib.av_hwdevice_ctx_create(&raw_device,
                      settings.device == FFmpeg::Device::CUDA ? AV_HWDEVICE_TYPE_CUDA : AV_HWDEVICE_TYPE_VAAPI,
                      path, nullptr, 0), "create hardware device");
            device.reset(raw_device);
            pool.reset(lib.av_hwframe_ctx_alloc(device.get()));
            if (!pool) throw std::bad_alloc();
            auto *frames = reinterpret_cast<AVHWFramesContext *>(pool->data);
            frames->format = context->pix_fmt;
            frames->sw_format = AV_PIX_FMT_NV12;
            frames->width = settings.width;
            frames->height = settings.height;
            frames->initial_pool_size = 20;
            check(lib.av_hwframe_ctx_init(pool.get()), "initialize hardware frames");
            context->hw_frames_ctx = lib.av_buffer_ref(pool.get());
            if (!context->hw_frames_ctx) throw std::bad_alloc();
            hw_frame.reset(lib.av_frame_alloc());
            if (!hw_frame) throw std::bad_alloc();
        }
        frame->format = hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
        frame->width = settings.width;
        frame->height = settings.height;
        check(lib.av_frame_get_buffer(frame.get(), 0), "allocate frame buffer");
        check(lib.avcodec_open2(context.get(), codec, nullptr), "open encoder");
    }
    void encode_argb(const uint8_t *data, int stride, int64_t pts, bool keyframe) override {
        if (!data || stride < int64_t(settings.width) * 4)
            throw std::invalid_argument("Invalid ARGB input or row stride");
        check(lib.av_frame_make_writable(frame.get()), "make frame writable");
        const bool hardware = settings.device != FFmpeg::Device::Software;
        const int result = hardware
            ? libyuv::ARGBToNV12(data, stride, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                settings.width, settings.height)
            : libyuv::ARGBToI420(data, stride, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                frame->data[2], frame->linesize[2], settings.width, settings.height);
        if (result) throw std::runtime_error("libyuv pixel conversion failed");
        frame->pts = pts;
        frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        AVFrame *input = frame.get();
        if (hardware) {
            // Do not overwrite a surface retained asynchronously by the encoder.
            lib.av_frame_unref(hw_frame.get());
            check(lib.av_hwframe_get_buffer(pool.get(), hw_frame.get(), 0), "allocate hardware frame");
            check(lib.av_hwframe_transfer_data(hw_frame.get(), frame.get(), 0), "upload hardware frame");
            hw_frame->pts = pts;
            hw_frame->pict_type = frame->pict_type;
            input = hw_frame.get();
        }
        int sent = lib.avcodec_send_frame(context.get(), input);
        if (sent == AVERROR(EAGAIN)) {
            receive();
            sent = lib.avcodec_send_frame(context.get(), input);
        }
        check(sent, "submit video frame");
        receive();
    }
    FFmpeg::Packet packet() const override {
        if (packets.empty()) return {};
        const auto &p = packets.front();
        return {p->data, static_cast<size_t>(p->size), bool(p->flags & AV_PKT_FLAG_KEY), p->pts};
    }
    void pop_packet() override { if (!packets.empty()) packets.pop_front(); }
};

class FrameSource final : public FFmpeg::FrameSource {
    Library &lib = Library::get();
    std::string input_path;
    int scaled_format = -1;
    Library::FormatCtxGuard format;
    Library::ContextGuard decoder;
    Library::FrameGuard frame;
    Library::FrameGuard rgb;
    Library::PacketGuard packet;
    Library::SwsContextGuard scaler;
    int stream = -1;
    bool draining = false;
public:
    explicit FrameSource(const std::string &path) : input_path(path) {
        AVFormatContext *raw{};
        check(lib.avformat_open_input(&raw, path.c_str(), nullptr, nullptr), "open benchmark input");
        format.reset(raw);
        check(lib.avformat_find_stream_info(format.get(), nullptr), "read stream metadata");
        for (unsigned i = 0; i < format->nb_streams; ++i)
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { stream = i; break; }
        if (stream < 0) throw std::runtime_error("No video stream in benchmark input");
        auto *parameters = format->streams[stream]->codecpar;
        const auto *codec = lib.avcodec_find_decoder(parameters->codec_id);
        if (!codec) throw std::runtime_error("Benchmark decoder unavailable");
        decoder.reset(lib.avcodec_alloc_context3(codec));
        frame.reset(lib.av_frame_alloc());
        rgb.reset(lib.av_frame_alloc());
        packet.reset(lib.av_packet_alloc());
        if (!decoder || !frame || !rgb || !packet) throw std::bad_alloc();
        check(lib.avcodec_parameters_to_context(decoder.get(), parameters), "configure decoder");
        check(lib.avcodec_open2(decoder.get(), codec, nullptr), "open decoder");
    }
    int width() const override { return decoder->width; }
    int height() const override { return decoder->height; }
    int64_t frame_count() const override { return format->streams[stream]->nb_frames; }
    bool next(FFmpeg::RGBFrame &output) override {
        for (;;) {
            int result = lib.avcodec_receive_frame(decoder.get(), frame.get());
            if (!result) {
                if (!scaler || rgb->width != frame->width || rgb->height != frame->height || scaled_format != frame->format) {
                    scaler.reset(lib.sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                    frame->width, frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr));
                    if (!scaler) throw std::runtime_error("Cannot create benchmark scaler");
                    scaled_format = frame->format;
                    lib.av_frame_unref(rgb.get());
                    rgb->format = AV_PIX_FMT_RGB24;
                    rgb->width = frame->width;
                    rgb->height = frame->height;
                    check(lib.av_frame_get_buffer(rgb.get(), 0), "allocate benchmark RGB frame");
                }
                check(lib.sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height,
                                    rgb->data, rgb->linesize), "convert benchmark frame");
                output = {rgb->data[0], rgb->linesize[0], rgb->width, rgb->height};
                return true;
            }
            if (result == AVERROR_EOF) return false;
            check(result == AVERROR(EAGAIN) ? 0 : result, "decode benchmark frame");
            if (draining) throw std::runtime_error("Decoder requested input while draining");
            do {
                lib.av_packet_unref(packet.get());
                result = lib.av_read_frame(format.get(), packet.get());
            } while (result >= 0 && packet->stream_index != stream);
            if (result == AVERROR_EOF) {
                check(lib.avcodec_send_packet(decoder.get(), nullptr), "drain benchmark decoder");
                draining = true;
            } else {
                check(result, "read benchmark packet");
                // Receive was drained to EAGAIN, so send must now accept input.
                check(lib.avcodec_send_packet(decoder.get(), packet.get()), "submit benchmark packet");
            }
        }
    }
    void rewind() override {
        if (lib.av_seek_frame(format.get(), stream, 0, AVSEEK_FLAG_BACKWARD) < 0) {
            // Raw elementary streams need not have seekable timestamps. Reopen
            // the same input to support deterministic benchmark replay as well.
            FrameSource fresh(input_path);
            format.swap(fresh.format);
            decoder.swap(fresh.decoder);
            frame.swap(fresh.frame);
            rgb.swap(fresh.rgb);
            packet.swap(fresh.packet);
            scaler.swap(fresh.scaler);
            std::swap(stream, fresh.stream);
            std::swap(scaled_format, fresh.scaled_format);
            draining = false;
            return;
        }
        lib.avcodec_flush_buffers(decoder.get());
        lib.av_packet_unref(packet.get());
        draining = false;
    }
};

class Backend final : public FFmpegBackend {
public:
    unsigned major() const override { return LIBAVCODEC_VERSION_MAJOR; }
    std::string description() const override { return Library::get().description(); }
    std::unique_ptr<FFmpeg::Encoder> create_encoder(const FFmpeg::EncoderSettings &settings) override {
        return std::make_unique<Encoder>(settings);
    }
    std::unique_ptr<FFmpeg::FrameSource> open_source(const std::string &path) override {
        return std::make_unique<FrameSource>(path);
    }
};
} // namespace KASMVNC_FFMPEG_NAMESPACE

std::unique_ptr<FFmpegBackend> KASMVNC_FFMPEG_FACTORY() {
    if (!KASMVNC_FFMPEG_NAMESPACE::Library::get().is_available()) return {};
    return std::make_unique<KASMVNC_FFMPEG_NAMESPACE::Backend>();
}

/* Copyright (C) 2026 Kasm Technologies Corp. GPL-2.0-or-later. */
#include "Backend.h"
#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <libyuv.h>
#include <new>
#include <stdexcept>
#include <type_traits>
// The symbol table and every use of FFmpeg structures belong to this versioned
// translation unit. The public interface uses only Kasm-owned types.
#include "Library.cxx.inc"

#if KASMVNC_EXPECTED_AVCODEC
static_assert(
    LIBAVCODEC_VERSION_MAJOR == KASMVNC_EXPECTED_AVCODEC, "FFmpeg adapter compiled with the wrong header family");
#endif

namespace KASMVNC_FFMPEG {
    class Backend final : public FFmpegBackend {
    public:
        inline static thread_local FFmpeg::ProbeCategory last_error = FFmpeg::ProbeCategory::Ok;

        static void check(int result, const char *operation,
                          FFmpeg::ProbeCategory category = FFmpeg::ProbeCategory::Error) {
            if (result >= 0)
                return;

            if (last_error == FFmpeg::ProbeCategory::Ok)
                last_error = category;

            throw std::runtime_error(fmt::format("{}: {}", operation, Library::get().get_error_description(result)));
        }

        unsigned major() const override {
            return LIBAVCODEC_VERSION_MAJOR;
        }

        std::string description() const override {
            return Library::get().description();
        }

        bool has_encoder(const std::string &name) const override {
            const auto &lib = Library::get();
            const auto *codec = lib.avcodec_find_encoder_by_name(name.c_str());
            return codec && codec->type == AVMEDIA_TYPE_VIDEO && lib.av_codec_is_encoder(codec);
        }

        FFmpeg::ProbeCategory get_last_error() const override {
            return last_error;
        }

        std::unique_ptr<FFmpeg::Encoder> create_encoder(const FFmpeg::EncoderSettings &settings) override;

        std::unique_ptr<FFmpeg::FrameSource> open_source(const std::string &path) override;
    };

    struct NoHardware {
    };

    struct HardwareState {
        Library::BufferGuard device;
        Library::BufferGuard pool;
        Library::FrameGuard frame;
    };

    template<FFmpeg::Device D>
    class Encoder final : public FFmpeg::Encoder {
        static constexpr bool hardware = D != FFmpeg::Device::Software;
        static constexpr AVPixelFormat codec_format = D == FFmpeg::Device::CUDA
                                                          ? AV_PIX_FMT_CUDA
                                                          : D == FFmpeg::Device::VAAPI
                                                                ? AV_PIX_FMT_VAAPI
                                                                : AV_PIX_FMT_YUV420P;
        static constexpr AVPixelFormat frame_format = hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;

        Library &lib = Library::get();
        FFmpeg::EncoderSettings settings;
        Library::ContextGuard context;
        Library::FrameGuard frame;
        [[no_unique_address]] std::conditional_t<hardware, HardwareState, NoHardware> hw;
        Library::PacketGuard pending;
        bool has_packet{};
        bool keyframe_pending{};
        int64_t pts_counter{};

        void option(const char *name, const char *value) {
            const int result = lib.av_opt_set(context->priv_data, name, value, 0);
            if (result < 0) {
                Backend::last_error = FFmpeg::ProbeCategory::Option;
                vlog.info("%s: cannot set %s=%s (%s)",
                          settings.name.c_str(),
                          name,
                          value,
                          lib.get_error_description(result).c_str());
            }
        }

        void option(const char *name, int value) {
            const int result = lib.av_opt_set_int(context->priv_data, name, value, 0);
            if (result < 0) {
                Backend::last_error = FFmpeg::ProbeCategory::Option;
                vlog.info("%s: cannot set %s=%d (%s)",
                          settings.name.c_str(),
                          name,
                          value,
                          lib.get_error_description(result).c_str());
            }
        }

        const AVOption *find_option(const char *name) {
            for (const AVOption *entry = nullptr; (entry = lib.av_opt_next(context->priv_data, entry));)
                if (std::string(entry->name) == name)
                    return entry;
            return nullptr;
        }

        void bounded_option(const char *name, int value) {
            // Runtime option limits can change within an ABI family. For example,
            // AV1 NVENC cq accepts 0..51 in FFmpeg 6.1 and 0..63 in FFmpeg 7.1.
            if (const auto *entry = find_option(name)) {
                const int bounded = std::clamp(value, int(entry->min), int(entry->max));
                if (bounded != value)
                    vlog.info("%s: clamping %s=%d to runtime limit %d", settings.name.c_str(), name, value, bounded);
                option(name, bounded);
                return;
            }
            option(name, value);
        }

        bool failed(int result, const char *operation) const {
            if (result >= 0)
                return false;
            vlog.error("%s: %s: %s", settings.name.c_str(), operation, lib.get_error_description(result).c_str());
            return true;
        }

        void drop_packet() {
            lib.av_packet_unref(pending.get());
            has_packet = false;
        }

        void set_software_options() {
            if (settings.name == "libsvtav1") {
                option("preset", "12");
                option("svtav1-params", "rtc=1");
            } else {
                option("tune", "zerolatency");
                option("preset", "ultrafast");
            }
            option("crf", settings.quality);
        }

        void set_cuda_options() {
            context->refs = 0;
            option("rc", "vbr");
            // FFmpeg 4.4 and 5.1 can use pre-SDK-10 NVENC headers, where
            // p4/tune do not exist. Detect runtime capabilities rather than
            // assuming an SDK version from the libavcodec ABI family.
            if (!find_option("tune")) {
                option("preset", "llhq");
            } else {
                option("preset", "p4");
                option("tune", "ull");
            }
            option("zerolatency", 1);
            option("b_ref_mode", 0);
            option("delay", "0");
            // qmin/qmax are generic codec options, not private NVENC options.
            context->qmin = settings.quality;
            context->qmax = settings.maximum_quality;
            // cq is not the same scale as AV1's qmin/qmax (0..255).
            bounded_option("cq", settings.quality);
        }

        void set_vaapi_options(const AVCodec *codec) {
#if LIBAVCODEC_VERSION_MAJOR < 59
            // FFmpeg 4.4 VAAPI is synchronous and has no async_depth.
            // Honor the option if a distro backported asynchronous encode.
            if (find_option("async_depth"))
                option("async_depth", "1");
#else
            option("async_depth", "1");
#endif
            option("rc_mode", "CQP");
            if (codec->id == AV_CODEC_ID_AV1) {
                // AV1 VAAPI has no private qp option. CQP reads this value
                // directly when AV_CODEC_FLAG_QSCALE is not set.
                context->global_quality = settings.quality;
            } else {
                option("qp", settings.quality);
            }
        }

        const char *device_path() const {
            if (settings.device_path.empty())
                return nullptr;
            if constexpr (D == FFmpeg::Device::CUDA) {
                const bool ordinal = std::all_of(settings.device_path.begin(),
                                                 settings.device_path.end(),
                                                 [](unsigned char c) { return std::isdigit(c); });
                if (!ordinal) {
                    vlog.info("%s: ignoring non-ordinal CUDA device '%s'; using the default device",
                              settings.name.c_str(),
                              settings.device_path.c_str());
                    return nullptr;
                }
            }
            return settings.device_path.c_str();
        }

        void create_hardware_frames() {
            constexpr auto type = D == FFmpeg::Device::CUDA ? AV_HWDEVICE_TYPE_CUDA : AV_HWDEVICE_TYPE_VAAPI;
            AVBufferRef *raw_device{};
            Backend::check(lib.av_hwdevice_ctx_create(&raw_device, type, device_path(), nullptr, 0),
                           "create hardware device",
                           FFmpeg::ProbeCategory::Hardware);
            hw.device.reset(raw_device);
            hw.pool.reset(lib.av_hwframe_ctx_alloc(hw.device.get()));
            if (!hw.pool)
                throw std::bad_alloc();
            auto *frames = reinterpret_cast<AVHWFramesContext *>(hw.pool->data);
            frames->format = codec_format;
            frames->sw_format = frame_format;
            frames->width = settings.width;
            frames->height = settings.height;
            frames->initial_pool_size = 20;
            Backend::check(lib.av_hwframe_ctx_init(hw.pool.get()), "initialize hardware frames",
                           FFmpeg::ProbeCategory::Hardware);
            context->hw_frames_ctx = lib.av_buffer_ref(hw.pool.get());
            if (!context->hw_frames_ctx)
                throw std::bad_alloc();
            hw.frame.reset(lib.av_frame_alloc());
            if (!hw.frame)
                throw std::bad_alloc();
        }

    public:
        explicit Encoder(const FFmpeg::EncoderSettings &value) : settings(value), pts_counter(value.first_pts) {
            if (settings.width < 2 || settings.height < 2 || settings.width % 2 || settings.height % 2 ||
                settings.frame_rate <= 0 || settings.gop < 0)
                throw std::invalid_argument("Invalid encoder dimensions or frame timing");
            const auto *codec = lib.avcodec_find_encoder_by_name(settings.name.c_str());
            if (!codec || codec->type != AVMEDIA_TYPE_VIDEO || !lib.av_codec_is_encoder(codec))
                throw std::runtime_error(fmt::format("Encoder is not available: {}", settings.name));
            context.reset(lib.avcodec_alloc_context3(codec));
            frame.reset(lib.av_frame_alloc());
            pending.reset(lib.av_packet_alloc());
            if (!context || !frame || !pending)
                throw std::bad_alloc();
            context->time_base = {1, settings.frame_rate};
            context->framerate = {settings.frame_rate, 1};
            context->gop_size = settings.gop;
            context->max_b_frames = 0;
            context->width = settings.width;
            context->height = settings.height;
            context->profile = settings.profile;
            context->pix_fmt = codec_format;
            if constexpr (D == FFmpeg::Device::Software) {
                set_software_options();
            } else {
                context->flags |= AV_CODEC_FLAG_LOW_DELAY;
                context->delay = 0;
                if constexpr (D == FFmpeg::Device::CUDA)
                    set_cuda_options();
                else
                    set_vaapi_options(codec);
                create_hardware_frames();
            }
            frame->format = frame_format;
            frame->width = settings.width;
            frame->height = settings.height;
            Backend::check(lib.av_frame_get_buffer(frame.get(), 0), "allocate frame buffer");
            Backend::check(lib.avcodec_open2(context.get(), codec, nullptr), "open encoder");
        }

        int64_t next_pts() const override { return pts_counter; }

        bool encode_argb(const uint8_t *data, int stride, bool keyframe) override {
            if (!data || stride < int64_t(settings.width) * 4) {
                vlog.error("%s: invalid ARGB input or row stride", settings.name.c_str());
                return false;
            }
            if (has_packet) {
                drop_packet();
                keyframe = true;
            }
            keyframe = keyframe || keyframe_pending;
            keyframe_pending = keyframe;
            int result;
            if constexpr (hardware)
                result = libyuv::ARGBToNV12(data,
                                            stride,
                                            frame->data[0],
                                            frame->linesize[0],
                                            frame->data[1],
                                            frame->linesize[1],
                                            settings.width,
                                            settings.height);
            else
                result = libyuv::ARGBToI420(data,
                                            stride,
                                            frame->data[0],
                                            frame->linesize[0],
                                            frame->data[1],
                                            frame->linesize[1],
                                            frame->data[2],
                                            frame->linesize[2],
                                            settings.width,
                                            settings.height);
            if (result) {
                vlog.error("%s: libyuv pixel conversion failed (%d)", settings.name.c_str(), result);
                return false;
            }

            const int64_t pts = pts_counter++;
            frame->pts = pts;
            frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
            AVFrame *input = frame.get();

            if constexpr (hardware) {
                // Do not overwrite a surface retained asynchronously by the encoder.
                lib.av_frame_unref(hw.frame.get());
                if (failed(lib.av_hwframe_get_buffer(hw.pool.get(), hw.frame.get(), 0), "allocate hardware frame") ||
                    failed(lib.av_hwframe_transfer_data(hw.frame.get(), frame.get(), 0), "upload hardware frame"))
                    return false;
                hw.frame->pts = pts;
                hw.frame->pict_type = frame->pict_type;
                input = hw.frame.get();
            }

            if (failed(lib.avcodec_send_frame(context.get(), input), "submit video frame"))
                return false;

            keyframe_pending = false;

            int received = lib.avcodec_receive_packet(context.get(), pending.get());
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
                lib.avcodec_send_frame(context.get(), input);
                received = lib.avcodec_receive_packet(context.get(), pending.get());
                if (received == AVERROR(EAGAIN)) {
                    vlog.error("%s: encoder buffering frame (EAGAIN)", settings.name.c_str());
                    return false;
                }
            }

            if (received == AVERROR_EOF) {
                vlog.error("%s: encoder EOF reached", settings.name.c_str());
                return false;
            }
            if (failed(received, "receive encoded packet"))
                return false;

            has_packet = true;
            return true;
        }

        FFmpeg::Packet packet() const override {
            if (!has_packet)
                return {};
            return {
                pending->data,
                static_cast<size_t>(pending->size),
                bool(pending->flags & AV_PKT_FLAG_KEY),
                pending->pts
            };
        }

        void pop_packet() override {
            if (has_packet)
                drop_packet();
        }
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
            Backend::check(lib.avformat_open_input(&raw, path.c_str(), nullptr, nullptr), "open benchmark input");
            format.reset(raw);
            Backend::check(lib.avformat_find_stream_info(format.get(), nullptr), "read stream metadata");
            for (unsigned i = 0; i < format->nb_streams; ++i)
                if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    stream = i;
                    break;
                }
            if (stream < 0)
                throw std::runtime_error("No video stream in benchmark input");
            auto *parameters = format->streams[stream]->codecpar;
            const auto *codec = lib.avcodec_find_decoder(parameters->codec_id);
            if (!codec)
                throw std::runtime_error("Benchmark decoder unavailable");
            decoder.reset(lib.avcodec_alloc_context3(codec));
            frame.reset(lib.av_frame_alloc());
            rgb.reset(lib.av_frame_alloc());
            packet.reset(lib.av_packet_alloc());
            if (!decoder || !frame || !rgb || !packet)
                throw std::bad_alloc();
            Backend::check(lib.avcodec_parameters_to_context(decoder.get(), parameters), "configure decoder");
            Backend::check(lib.avcodec_open2(decoder.get(), codec, nullptr), "open decoder");
        }

        int width() const override {
            return decoder->width;
        }

        int height() const override {
            return decoder->height;
        }

        int64_t frame_count() const override {
            return format->streams[stream]->nb_frames;
        }

        bool next(FFmpeg::RGBFrame &output) override {
            for (;;) {
                int result = lib.avcodec_receive_frame(decoder.get(), frame.get());
                if (!result) {
                    if (!scaler || rgb->width != frame->width || rgb->height != frame->height ||
                        scaled_format != frame->format) {
                        scaler.reset(lib.sws_getContext(frame->width,
                                                        frame->height,
                                                        static_cast<AVPixelFormat>(frame->format),
                                                        frame->width,
                                                        frame->height,
                                                        AV_PIX_FMT_RGB24,
                                                        SWS_BILINEAR,
                                                        nullptr,
                                                        nullptr,
                                                        nullptr));
                        if (!scaler)
                            throw std::runtime_error("Cannot create benchmark scaler");
                        scaled_format = frame->format;
                        lib.av_frame_unref(rgb.get());
                        rgb->format = AV_PIX_FMT_RGB24;
                        rgb->width = frame->width;
                        rgb->height = frame->height;
                        Backend::check(lib.av_frame_get_buffer(rgb.get(), 0), "allocate benchmark RGB frame");
                    }
                    Backend::check(lib.sws_scale(
                                       scaler.get(), frame->data, frame->linesize, 0, frame->height, rgb->data,
                                       rgb->linesize),
                                   "convert benchmark frame");
                    output = {rgb->data[0], rgb->linesize[0], rgb->width, rgb->height};
                    return true;
                }
                if (result == AVERROR_EOF)
                    return false;
                Backend::check(result == AVERROR(EAGAIN) ? 0 : result, "decode benchmark frame");
                if (draining)
                    throw std::runtime_error("Decoder requested input while draining");
                do {
                    lib.av_packet_unref(packet.get());
                    result = lib.av_read_frame(format.get(), packet.get());
                } while (result >= 0 && packet->stream_index != stream);
                if (result == AVERROR_EOF) {
                    Backend::check(lib.avcodec_send_packet(decoder.get(), nullptr), "drain benchmark decoder");
                    draining = true;
                } else {
                    Backend::check(result, "read benchmark packet");
                    // Receive was drained to EAGAIN, so send must now accept input.
                    Backend::check(lib.avcodec_send_packet(decoder.get(), packet.get()), "submit benchmark packet");
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

    std::unique_ptr<FFmpeg::Encoder> Backend::create_encoder(const FFmpeg::EncoderSettings &settings) {
        last_error = FFmpeg::ProbeCategory::Ok;
        switch (settings.device) {
            case FFmpeg::Device::VAAPI:
                return std::make_unique<Encoder<FFmpeg::Device::VAAPI> >(settings);
            case FFmpeg::Device::CUDA:
                return std::make_unique<Encoder<FFmpeg::Device::CUDA> >(settings);
            case FFmpeg::Device::Software:
                break;
        }
        return std::make_unique<Encoder<FFmpeg::Device::Software> >(settings);
    }

    std::unique_ptr<FFmpeg::FrameSource> Backend::open_source(const std::string &path) {
        return std::make_unique<FrameSource>(path);
    }
} // namespace KASMVNC_FFMPEG

FFmpegBackend *KASMVNC_FFMPEG_FACTORY() {
    if (!KASMVNC_FFMPEG::Library::get().is_available())
        return nullptr;

    static KASMVNC_FFMPEG::Backend instance;
    return &instance;
}

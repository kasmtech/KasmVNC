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
#include "FFMPEGHWEncoder.h"

#include <fmt/format.h>
#include <rfb/ServerCore.h>

#include "EncoderProbe.h"
#include "rfb/LogWriter.h"

#include "EncoderConfiguration.h"
#include "KasmVideoConstants.h"
#include "rfb/encodings.h"
#include <rfb/encoders/utils.h>
#include <libyuv.h>

static rfb::LogWriter vlog("FFMPEGHWEncoder");

namespace rfb {
    template<AVHWDeviceType HWDeviceType, AVPixelFormat AVPixFmt>
    FFMPEGHWEncoder<HWDeviceType, AVPixFmt>::FFMPEGHWEncoder(Screen layout_, const FFmpeg &ffmpeg_, SConnection *conn, KasmVideoEncoders::Encoder encoder_,
        const char *dri_node_, VideoEncoderParams params) :
        VideoEncoder(layout_.id, conn), layout(layout_),
        ffmpeg(ffmpeg_), encoder(encoder_), current_params(params), msg_codec_id(KasmVideoEncoders::to_msg_id(encoder)),
        msg_codec_type_id(pseudoEncodingStreamingModeJpegWebp - KasmVideoEncoders::to_encoding(encoder)), dri_node(dri_node_) {
        AVBufferRef *hw_device_ctx{};
        int err{};

        DEBUG_LOG(vlog, "Constructor: HWDeviceType=%d, AVPixFmt=%d, encoder=%s, dri_node=%s",
                   HWDeviceType, AVPixFmt, KasmVideoEncoders::to_string(encoder),
                   dri_node_ ? dri_node_ : "null");

        if (err = ffmpeg.av_hwdevice_ctx_create(&hw_device_ctx, HWDeviceType, dri_node_, nullptr, 0); err < 0) {
            throw std::runtime_error(fmt::format("Failed to create hw device context {}", ffmpeg.get_error_description(err)));
        }

        hw_device_ctx_guard.reset(hw_device_ctx);
        const auto *enc_name = KasmVideoEncoders::to_string(encoder);
        DEBUG_LOG(vlog, "Looking for encoder: %s", enc_name);
        codec = ffmpeg.avcodec_find_encoder_by_name(enc_name);
        if (!codec)
            throw std::runtime_error(fmt::format("Could not find {} encoder", enc_name));

        auto *frame = ffmpeg.av_frame_alloc();
        if (!frame)
            throw std::runtime_error("Cannot allocate AVFrame");

        sw_frame_guard.reset(frame);

        auto *pkt = ffmpeg.av_packet_alloc();
        if (!pkt) {
            throw std::runtime_error("Could not allocate packet");
        }
        pkt_guard.reset(pkt);
    }

    template<AVHWDeviceType HWDeviceType, AVPixelFormat AVPixFmt>
    bool FFMPEGHWEncoder<HWDeviceType, AVPixFmt>::init(int width, int height, VideoEncoderParams params) {
        current_params = params;
        AVHWFramesContext *frames_ctx{};
        int err{};

        vlog.debug("FRAME RESIZE (%d, %d): RATE: %d, GOP: %d, QUALITY: %d", width, height, current_params.frame_rate, current_params.group_of_picture, current_params.quality);

        auto *ctx = ffmpeg.avcodec_alloc_context3(codec);
        if (!ctx) {
            vlog.error("Cannot allocate AVCodecContext");
            return false;
        }
        ctx_guard.reset(ctx);

        ctx->time_base = {1, current_params.frame_rate};
        ctx->framerate = {current_params.frame_rate, 1};
        ctx->gop_size = current_params.group_of_picture; // interval between I-frames
        ctx->max_b_frames = 0; // No B-frames for immediate output
        ctx->pix_fmt = AVPixFmt;
        ctx->width = current_params.width;
        ctx->height = current_params.height;
        ctx->delay = 0;
        ctx->profile = EncoderConfiguration::get_configuration(encoder).profile;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

        DEBUG_LOG(vlog, "Encoder context config: pix_fmt=%d, width=%d, height=%d, coded_width=%d, coded_height=%d, framerate=%d, gop=%d, quality=%d",
                   ctx->pix_fmt, ctx->width, ctx->height, ctx->coded_width, ctx->coded_height,
                   current_params.frame_rate, current_params.group_of_picture, current_params.quality);

        if constexpr (HWDeviceType == AV_HWDEVICE_TYPE_CUDA && AVPixFmt == AV_PIX_FMT_CUDA) {
            // NVENC low-latency settings
            // Rate control mode: Constant Bit Rate (CBR)
            if (ffmpeg.av_opt_set(ctx->priv_data, "rc", "vbr", 0) < 0) {
                vlog.info("Cannot set rc to cbr");
            }

            ctx->refs = 0; // num_ref_frames=0 in SPS
            ctx->delay = 0;

            if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "p4", 0) < 0) {
                vlog.info("Cannot set preset to p1");
            }

            if (ffmpeg.av_opt_set(ctx->priv_data, "tune", "ull", 0) < 0) {
                vlog.info("Cannot set tune to ull (ultra-low-latency)");
            }

            if (ffmpeg.av_opt_set_int(ctx->priv_data, "zerolatency", 1, 0) < 0) {
                vlog.info("Cannot set zerolatency");
            }

            if (ffmpeg.av_opt_set_int(ctx->priv_data, "b_ref_mode", 0, 0) < 0) {
                vlog.info("Cannot set b_ref_mode");
            }

            // Set delay to 0
            if (ffmpeg.av_opt_set(ctx->priv_data, "delay", "0", 0) < 0) {
                vlog.info("Cannot set delay to 0");
            }

            const auto config = EncoderConfiguration::get_configuration(encoder);

            if (ffmpeg.av_opt_set_int(ctx->priv_data, "qmin", current_params.quality, 0) < 0) {
                vlog.info("Cannot set qmin");
            }

            if (ffmpeg.av_opt_set_int(ctx->priv_data, "qmax", config.allowed_quality.max, 0) < 0) {
                vlog.info("Cannot set qmax");
            }

            if (ffmpeg.av_opt_set_int(ctx->priv_data, "cq", current_params.quality, 0) < 0) {
                vlog.info("Cannot set cq");
            }

            /*
            // Disable temporal AQ
            if (ffmpeg.av_opt_set(ctx->priv_data, "temporal-aq", "1", 0) < 0) {
                vlog.info("Cannot disable temporal-aq");
            }

            // Disable spatial AQ
            if (ffmpeg.av_opt_set(ctx->priv_data, "spatial-aq", "1", 0) < 0) {
                vlog.info("Cannot disable spatial-aq");
            }*/

            // Disable SEI metadata to reduce bitstream overhead
            /*if (ffmpeg.av_opt_set_int(ctx->priv_data, "extra_sei", 0, 0) < 0) {
                vlog.info("Cannot set extra_sei (may not be supported)");
            }
            if (ffmpeg.av_opt_set_int(ctx->priv_data, "udu_sei", 0, 0) < 0) {
                vlog.info("Cannot set udu_sei (may not be supported)");
            }*/
             /*  // Set DPB size to 0
            if (ffmpeg.av_opt_set(ctx->priv_data, "dpb_size", "0", 0) < 0) {
                vlog.info("Cannot set dpb_size to 0");
            }*/
        } else {
            if (ffmpeg.av_opt_set(ctx->priv_data, "async_depth", "1", 0) < 0) {
                vlog.info("Cannot set async_depth");
            }

            if (ffmpeg.av_opt_set(ctx->priv_data, "rc_mode", "CQP", 0) < 0) {
                vlog.info("Cannot set rc_mode");
            }

            if (ffmpeg.av_opt_set_int(ctx->priv_data, "qp", current_params.quality, 0) < 0) {
                vlog.info("Cannot set qp");
            }
        }

        auto *hw_frames_ctx = ffmpeg.av_hwframe_ctx_alloc(hw_device_ctx_guard.get());
        if (!hw_frames_ctx) {
            vlog.error("Failed to create HW frame context");
            return false;
        }

        hw_frames_ref_guard.reset(hw_frames_ctx);

        frames_ctx = reinterpret_cast<AVHWFramesContext *>(hw_frames_ctx->data);
        frames_ctx->format = AVPixFmt;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = current_params.width;
        frames_ctx->height = current_params.height;
        frames_ctx->initial_pool_size = 20;

        DEBUG_LOG(vlog, "HW frame context config: format=%d (AVPixFmt template), sw_format=%d (NV12), width=%d, height=%d, pool_size=20",
                   frames_ctx->format, frames_ctx->sw_format, frames_ctx->width, frames_ctx->height);

        if (err = ffmpeg.av_hwframe_ctx_init(hw_frames_ctx); err < 0) {
            vlog.error("Failed to initialize HW frame context (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }
        DEBUG_LOG(vlog, "HW frame context initialized successfully");

        FFmpeg::av_buffer_unref(&ctx_guard->hw_frames_ctx);

        ctx_guard->hw_frames_ctx = ffmpeg.av_buffer_ref(hw_frames_ctx);
        if (!ctx_guard->hw_frames_ctx) {
            vlog.error("Failed to create buffer reference");
            return false;
        }

        auto *frame = ffmpeg.av_frame_alloc();
        if (!frame) {
            vlog.error("Cannot allocate AVFrame");
            return false;
        }
        sw_frame_guard.reset(frame);

        frame->format = AV_PIX_FMT_NV12;
        frame->width = params.width;
        frame->height = params.height;
        frame->pict_type = AV_PICTURE_TYPE_I;

        DEBUG_LOG(vlog, "SW frame config: format=%d (NV12), width=%d, height=%d", frame->format, frame->width, frame->height);

        if (ffmpeg.av_frame_get_buffer(frame, 0) < 0) {
            vlog.error("Could not allocate sw-frame data");
            return false;
        }

        DEBUG_LOG(vlog, "SW frame allocated: linesize[0]=%d, linesize[1]=%d", frame->linesize[0], frame->linesize[1]);

        auto *hw_frame = ffmpeg.av_frame_alloc();
        if (!hw_frame) {
            vlog.error("Cannot allocate hw AVFrame");
            return false;
        }
        hw_frame_guard.reset(hw_frame);

        if (err = ffmpeg.av_hwframe_get_buffer(hw_frames_ctx, hw_frame, 0); err < 0) {
            vlog.error("Could not allocate hw-frame data (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }

        DEBUG_LOG(vlog, "Opening codec: %s", codec->name);
        if (err = ffmpeg.avcodec_open2(ctx_guard.get(), codec, nullptr); err < 0) {
            vlog.error("Failed to open codec (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }
        DEBUG_LOG(vlog, "Codec opened successfully");

        return true;
    }

    template<AVHWDeviceType HWDeviceType, AVPixelFormat AVPixFmt>
    bool FFMPEGHWEncoder<HWDeviceType, AVPixFmt>::isSupported() const {
        return conn->cp.supportsEncoding(encodingKasmVideo);
    }

    template<AVHWDeviceType HWDeviceType, AVPixelFormat AVPixFmt>
    bool FFMPEGHWEncoder<HWDeviceType, AVPixFmt>::render(const PixelBuffer *pb, bool forceKeyFrame) {
        // compress
        int stride;
        const auto rect = layout.dimensions;
        const auto *buffer = pb->getBuffer(rect, &stride);

        const int width = rect.width();
        const int height = rect.height();
        auto *frame = sw_frame_guard.get();

        int dst_width = width;
        int dst_height = height;

        if (width % 2 != 0)
            dst_width = width & ~1;

        if (height % 2 != 0)
            dst_height = height & ~1;

        VideoEncoderParams params{dst_width,
            dst_height,
            static_cast<uint8_t>(Server::frameRate),
            static_cast<uint8_t>(Server::groupOfPicture),
            static_cast<uint8_t>(Server::videoQualityCRFCQP)};

        DEBUG_LOG(vlog, "render(): Creating VideoEncoderParams: width=%d, height=%d, frameRate=%d (Server::frameRate=%d), GOP=%d, quality=%d",
                  dst_width, dst_height, static_cast<uint8_t>(Server::frameRate), (int)Server::frameRate,
                  static_cast<uint8_t>(Server::groupOfPicture), static_cast<uint8_t>(Server::videoQualityCRFCQP));

        if (current_params != params) {
            bpp = pb->getPF().bpp >> 3;
            if (!init(width, height, params)) {
                vlog.error("Failed to initialize encoder");
                return false;
            }

            frame = sw_frame_guard.get();
        } else {
            frame->pict_type = AV_PICTURE_TYPE_NONE;
        }

        if (forceKeyFrame)
            frame->pict_type = AV_PICTURE_TYPE_I;

        const int src_stride_bytes = stride * bpp;

        DEBUG_LOG(vlog, "render(): width=%d, height=%d, dst_width=%d, dst_height=%d, stride=%d pixels, stride_bytes=%d, bpp=%d",
                   width, height, dst_width, dst_height, stride, src_stride_bytes, bpp);

        int err{};

        DEBUG_LOG(vlog, "Converting ARGB to NV12: src_stride=%d, dst_linesize[0]=%d, dst_linesize[1]=%d, dst_width=%d, dst_height=%d",
                   src_stride_bytes, frame->linesize[0], frame->linesize[1], dst_width, dst_height);

        if (err = libyuv::ARGBToNV12(buffer, src_stride_bytes, frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1], dst_width, dst_height); err != 0) {
            vlog.error("libyuv::ARGBToNV12 failed with code: %d", err);
            return false;
        }
        DEBUG_LOG(vlog, "ARGB to NV12 conversion successful");

        frame->pts = pts++;
        auto *hw_frame = hw_frame_guard.get();

        DEBUG_LOG(vlog, "SW frame before transfer: format=%d, width=%d, height=%d, linesize[0]=%d, linesize[1]=%d, pts=%ld",
                   frame->format, frame->width, frame->height, frame->linesize[0], frame->linesize[1], frame->pts);

        if (err = ffmpeg.av_hwframe_transfer_data(hw_frame, frame, 0); err < 0) {
            vlog.error(
                "Error while transferring frame data to surface (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }
        DEBUG_LOG(vlog, "Frame transfer successful");

        hw_frame->pts = frame->pts;
        hw_frame->pict_type = frame->pict_type;

        DEBUG_LOG(vlog, "HW frame before send: format=%d, width=%d, height=%d, linesize[0]=%d, linesize[1]=%d, pts=%ld",
                   hw_frame->format, hw_frame->width, hw_frame->height, hw_frame->linesize[0], hw_frame->linesize[1], hw_frame->pts);

        if (err = ffmpeg.avcodec_send_frame(ctx_guard.get(), hw_frame_guard.get()); err < 0) {
            vlog.error("Error sending frame to codec (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }
        DEBUG_LOG(vlog, "Frame sent to codec successfully");

        auto *pkt = pkt_guard.get();

        err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            // Trying again
            ffmpeg.avcodec_send_frame(ctx_guard.get(), hw_frame_guard.get());
            err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);

            if (err == AVERROR(EAGAIN)) {
                vlog.error("Encoder buffering frame (EAGAIN) - waiting for more input");

                return false;
            }
        }

        if (err == AVERROR_EOF) {
            vlog.error("Encoder EOF reached");
            return false;
        }

        if (err < 0) {
            vlog.error("Error receiving packet from codec: %d", err);
            return false;
        }

        if (pkt->flags & AV_PKT_FLAG_KEY) {
            DEBUG_LOG(vlog, "Key frame %ld", frame->pts);
        }

        return true;
    }

    template<AVHWDeviceType HWDeviceType, AVPixelFormat AVPixFmt>
    void FFMPEGHWEncoder<HWDeviceType, AVPixFmt>::writeRect(const PixelBuffer *pb, const Palette &palette) {
        auto *pkt = pkt_guard.get();
        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(layout.id);
        os->writeU8(msg_codec_id);
        os->writeU8(msg_codec_type_id);
        os->writeU8(pkt->flags & AV_PKT_FLAG_KEY);
        encoders::write_compact(os, pkt->size);
        os->writeBytes(&pkt->data[0], pkt->size);
        DEBUG_LOG(vlog, "Screen id %d, codec %d, frame size:  %d", layout.id, msg_codec_id, pkt->size);

        ffmpeg.av_packet_unref(pkt);
    }

    template<AVHWDeviceType HWDeviceType, AVPixelFormat AVPixFmt>
    void FFMPEGHWEncoder<HWDeviceType, AVPixFmt>::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {}

    template<AVHWDeviceType HWDeviceType, AVPixelFormat AVPixFmt>
    void FFMPEGHWEncoder<HWDeviceType, AVPixFmt>::writeSkipRect() {
        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(layout.id);
        os->writeU8(kasmVideoSkip);
    }

    template class FFMPEGHWEncoder<AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI>;
    template class FFMPEGHWEncoder<AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA>;
} // namespace rfb

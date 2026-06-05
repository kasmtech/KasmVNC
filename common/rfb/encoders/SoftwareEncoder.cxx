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
#include "SoftwareEncoder.h"
#include "KasmVideoConstants.h"
#include <rfb/LogWriter.h>
#include <rfb/SConnection.h>
#include <rfb/ServerCore.h>
#include <rfb/encodings.h>
#include <rfb/ffmpeg.h>
#include <fmt/format.h>
#include <rfb/encoders/utils.h>
#include <libyuv.h>
#include "EncoderConfiguration.h"

static rfb::LogWriter vlog("SoftwareEncoder");

namespace rfb {
    SoftwareEncoder::SoftwareEncoder(Screen layout_, const FFmpeg &ffmpeg_, SConnection *conn, KasmVideoEncoders::Encoder encoder_,
                                             VideoEncoderParams params) :
        VideoEncoder(layout_.id, conn), layout(layout_),
        ffmpeg(ffmpeg_), encoder(encoder_), current_params(params), msg_codec_id(KasmVideoEncoders::to_msg_id(encoder)),
        msg_codec_type_id(pseudoEncodingStreamingModeJpegWebp - KasmVideoEncoders::to_encoding(encoder)) {
        const auto *enc_name = KasmVideoEncoders::to_string(encoder);
        codec = ffmpeg.avcodec_find_encoder_by_name(enc_name);
        if (!codec)
            throw std::runtime_error(fmt::format("Could not find {} encoder", enc_name));

        auto *frame = ffmpeg.av_frame_alloc();
        if (!frame) {
            throw std::runtime_error("Cannot allocate AVFrame");
        }
        frame_guard.reset(frame);

        auto *pkt = ffmpeg.av_packet_alloc();
        if (!pkt)
            throw std::runtime_error("Could not allocate packet");

        pkt_guard.reset(pkt);
    }

    bool SoftwareEncoder::isSupported() const {
        return conn->cp.supportsEncoding(encodingKasmVideo);
    }

    bool SoftwareEncoder::render(const PixelBuffer *pb, bool forceKeyFrame) {
        // compress
        int stride;

        const auto rect = layout.dimensions;
        const auto *buffer = pb->getBuffer(rect, &stride);

        const int width = rect.width();
        const int height = rect.height();
        auto *frame = frame_guard.get();

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

        if (current_params != params) {
            bpp = pb->getPF().bpp >> 3;
            if (!init(width, height, params)) {
                vlog.error("Failed to initialize encoder");
                return false;
            }

            frame = frame_guard.get();
        } else {
            frame->pict_type = AV_PICTURE_TYPE_NONE;
        }

        if (forceKeyFrame)
            frame->pict_type = AV_PICTURE_TYPE_I;

        const int src_stride_bytes = stride * bpp;
        int err = libyuv::ARGBToI420(buffer,
            src_stride_bytes,
            frame->data[0],
            frame->linesize[0],
            frame->data[1],
            frame->linesize[1],
            frame->data[2],
            frame->linesize[2],
            dst_width,
            dst_height);
        if (err != 0) {
            vlog.error("libyuv::ARGBToI420 failed with code: %d", err);
            return false;
        }

        frame->pts = pts++;

        if (ffmpeg.avcodec_send_frame(ctx_guard.get(), frame) < 0) {
            vlog.error("Error sending frame to codec (%s). Error code: %d", ffmpeg.get_error_description(err).c_str(), err);
            return false;
        }

        auto *pkt = pkt_guard.get();

        err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            // Trying one more time
            err = ffmpeg.avcodec_send_frame(ctx_guard.get(), nullptr);
            err = ffmpeg.avcodec_receive_packet(ctx_guard.get(), pkt);
        }

        if (err < 0) {
            vlog.error("Error receiving packet from codec");
            writeSkipRect();
            return false;
        }

        if (pkt->flags & AV_PKT_FLAG_KEY)
            DEBUG_LOG(vlog, "Key frame %ld", frame->pts);

        return true;
    }

    void SoftwareEncoder::writeRect(const PixelBuffer *pb, const Palette &palette) {
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

    void SoftwareEncoder::writeSolidRect(int width, int height, const PixelFormat &pf, const rdr::U8 *colour) {}

    void SoftwareEncoder::writeSkipRect() {
        auto *os = conn->getOutStream(conn->cp.supportsUdp);
        os->writeU8(layout.id);
        os->writeU8(kasmVideoSkip);
    }

    bool SoftwareEncoder::init(int width, int height, VideoEncoderParams params) {
        current_params = params;
        vlog.debug("FRAME RESIZE (%d, %d): RATE: %d, GOP: %d, QUALITY: %d", width, height, current_params.frame_rate, current_params.group_of_picture, current_params.quality);

        auto *ctx = ffmpeg.avcodec_alloc_context3(codec);
        if (!ctx) {
            vlog.error("Cannot allocate AVCodecContext");
            return false;
        }

        ctx_guard.reset(ctx);

        ctx->time_base = {1, params.frame_rate};
        ctx->framerate = {params.frame_rate, 1};
        ctx->gop_size = params.group_of_picture; // interval between I-frames
        ctx->width = current_params.width;
        ctx->height = current_params.height;
        ctx->coded_width = current_params.width;
        ctx->coded_height = current_params.height;
        //  best
        // ctx->pix_fmt = AV_PIX_FMT_YUV444P; // AV_PIX_FMT_YUV420P;
        ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        ctx->max_b_frames = 0; // No B-frames for immediate output
        ctx->profile = EncoderConfiguration::get_configuration(encoder).profile;

        // HIGH
        // if (ffmpeg.av_opt_set(ctx->priv_data, "tune", "zerolatency,stillimage", 0) != 0)
        //     return false;
        //
        // // start here, lower (20–22) = better quality,
        // // higher (24–28) = lower bitrate
        // if (ffmpeg.av_opt_set(ctx->priv_data, "crf", "18", 0) != 0)
        //     return false;
        //
        // // Preset: speed vs. compression efficiency
        // if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "medium", 0) != 0)
        //     return false;

        if (ffmpeg.av_opt_set(ctx->priv_data, "tune", "zerolatency", 0) < 0) {
            vlog.info("Cannot set tune to zerolatency");
        }

        if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "ultrafast", 0) < 0) {
            vlog.info("Cannot set preset to ultrafast");
        }

        if (encoder == KasmVideoEncoders::Encoder::av1_software) {
            if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "12", 0) < 0) {
                vlog.info("Cannot set preset to 8");
            }

            if (ffmpeg.av_opt_set(ctx->priv_data, "svtav1-params", "rtc=1", 0) < 0) {
                vlog.info("Cannot set -svtav1-params to tune=0");
            }
        }

        // start here, lower (20–22) = better quality,
        // higher (24–28) = lower bitrate
        if (ffmpeg.av_opt_set_int(ctx->priv_data, "crf", current_params.quality, 0) < 0) {
            vlog.info("Cannot set crf to %d", current_params.quality);
        }


        // // Preset: speed vs. compression efficiency
        // if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "medium", 0) != 0)
        //     return false;

        /*if (ffmpeg.av_opt_set(ctx->priv_data, "preset", "ultrafast", 0) != 0)
            throw std::runtime_error("Could not set codec setting");*/
        // "ultrafast" = lowest latency but bigger bitrate
        // "veryfast" = good balance for realtime
        // "medium+" = too slow for live

        // H.264 profile for better compression
        // if (ffmpeg.av_opt_set(ctx->priv_data, "profile", "high", 0) != 0)
        //     throw std::runtime_error("Could not set codec setting");

        auto *frame = frame_guard.get();

        ffmpeg.av_frame_unref(frame);
        frame->format = ctx_guard->pix_fmt;
        frame->width = current_params.width;
        frame->height = current_params.height;
        frame->pict_type = AV_PICTURE_TYPE_I;

        if (ffmpeg.av_frame_get_buffer(frame, 0) < 0) {
            vlog.error("Could not allocate frame data");
            return false;
        }

        if (ffmpeg.avcodec_open2(ctx_guard.get(), codec, nullptr) < 0) {
            vlog.error("Failed to open codec");
            return false;
        }

        return true;
    }
} // namespace rfb

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

#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

struct AVFormatContextDeleter {
 void operator()(AVFormatContext *ctx) const {
  avformat_close_input(&ctx);
 }
};

struct AVCodecContextDeleter {
 void operator()(AVCodecContext *ctx) const {
  avcodec_free_context(&ctx);
 }
};

struct AVFrameDeleter {
 void operator()(AVFrame *frame) const {
  av_frame_free(&frame);
 }
};

struct SwsContextDeleter {
 void operator()(SwsContext *ctx) const {
  sws_freeContext(ctx);
 }
};

struct PacketDeleter {
 void operator()(AVPacket *packet) const {
  av_packet_free(&packet);
 }
};

using FormatCtxGuard = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using CodecCtxGuard = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using FrameGuard = std::unique_ptr<AVFrame, AVFrameDeleter>;
using SwsContextGuard = std::unique_ptr<SwsContext, SwsContextDeleter>;
using PacketGuard = std::unique_ptr<AVPacket, PacketDeleter>;

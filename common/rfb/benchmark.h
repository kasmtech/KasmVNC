/* Copyright (C) 2025 Kasm Web
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

#include "Timer.h"
#include <memory>
#include <rfb/VNCServer.h>

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

namespace rfb {
    class BenchmarkServer : public VNCServer, public Timer::Callback {
    public:
        bool handleTimeout(Timer *t) override;

        void add_changed(const Region &region) override;

        void add_copied(const Region &dest, const Point &delta) override;

        void blockUpdates() override;

        void unblockUpdates() override;

        void setPixelBuffer(PixelBuffer *pb, const ScreenSet &layout) override;

        void setPixelBuffer(PixelBuffer *pb) override;

        void setScreenLayout(const ScreenSet &layout) override;

        [[nodiscard]] PixelBuffer *getPixelBuffer() const override;

        void announceClipboard(bool available) override;

        void bell() override;

        void closeClients(const char *reason) override;

        void setCursor(int width, int height, const Point &hotspot, const rdr::U8 *cursorData, bool resizing) override;

        void setCursorPos(const Point &p, bool warped) override;

        void setName(const char *name) override;

        void setLEDState(unsigned state) override;
    };
}

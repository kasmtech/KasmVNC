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

#include <assert.h>
#include <memory>
#include <rdr/FileInStream.h>
#include <rfb/VNCServer.h>

#include "CConnection.h"
#include "CMsgReader.h"
#include "EncCache.h"
#include "EncodeManager.h"
#include "LogWriter.h"
#include "screenTypes.h"
#include "SMsgWriter.h"

namespace rdr {
    class FileInStream;
}

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

static rfb::LogWriter vlog("Benchmarking");

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
    class MockBufferStream : public rdr::BufferedInStream {
        bool fillBuffer(size_t maxSize, bool wait) override {
            return true;
        }
    };

    class MockStream final : public rdr::OutStream {
    public:
        MockStream() {
            offset = 0;
            ptr = buf;
            end = buf + sizeof(buf);
        }

    private:
        void overrun(size_t needed) override {
            assert(end >= ptr);
            if (needed > static_cast<size_t>(end - ptr))
                flush();
        }

    public:
        size_t length() override {
            flush();
            return offset;
        }

        void flush() override {
            offset += ptr - buf;
            ptr = buf;
        }

    private:
        ptrdiff_t offset;
        rdr::U8 buf[8192]{};
    };

    class MockSConnection final : public SConnection {
    public:
        MockSConnection() {
            setStreams(nullptr, &out);

            setWriter(new SMsgWriter(&cp, &out, &udps));
        }

        ~MockSConnection() override = default;

        void writeUpdate(const UpdateInfo &ui, const PixelBuffer *pb) {
            //manager.pruneLosslessRefresh(Region(pb->getRect()));

            bytes += out.length();
            udp_bytes += udps.length();

            cache.clear();

            manager.clearEncodingTime();
            if (!ui.is_empty()) {
                manager.writeUpdate(ui, pb, nullptr);
            } else {
                Region region{pb->getRect()};
                manager.writeLosslessRefresh(region, pb, nullptr, 2000);
            }
        }

        void writeNoDataUpdate() {
            if (!writer()->needNoDataUpdate())
                return;

            writer()->writeNoDataUpdate();
        }

        void WriteDataUpdate() {
            Region req, pending;

            if (req.is_empty())
                return;

            if (!pending.is_empty()) {
                UpdateInfo ui;
                // However, we might still be able to send a lossless refresh
                req.assign_subtract(pending);
                req.assign_subtract(ui.changed);
                req.assign_subtract(ui.copied);

                ui.changed.clear();
                ui.copied.clear();
            }
        }

        void writeFramebufferUpdate() {
            manager.clearEncodingTime();

            if (state() != RFBSTATE_NORMAL)
                return;

            // First, take care of any updates that cannot contain framebuffer data
            // changes.
            writeNoDataUpdate();

            // Then real data (if possible)
            WriteDataUpdate();
        }

        void setDesktopSize(int fb_width, int fb_height,
                            const ScreenSet &layout) override {
            cp.width = fb_width;
            cp.height = fb_height;
            cp.screenLayout = layout;

            writer()->writeExtendedDesktopSize(reasonServer, 0, cp.width, cp.height,
                                               cp.screenLayout);
        }

        void sendStats(const bool toClient) override {
        }

        [[nodiscard]] bool canChangeKasmSettings() const override {
            return false;
        }

        void udpUpgrade(const char *resp) override {
        }

        void udpDowngrade(const bool) override {
        }

        void subscribeUnixRelay(const char *name) override {
        }

        void unixRelay(const char *name, const rdr::U8 *buf, const unsigned len) override {
        }

        void handleFrameStats(rdr::U32 all, rdr::U32 render) override {
        }

        [[nodiscard]] auto getJpegStats() const {
            return manager.jpegstats;
        }

        [[nodiscard]] auto getWebPStats() const {
            return manager.webpstats;
        }

        uint64_t bytes{};
        uint64_t udp_bytes{};

    protected:
        MockStream out{};
        MockStream udps{};

        EncCache cache{};
        EncodeManager manager{this, &cache};
    };

    static constexpr rdr::S32 encodings[] = {
        encodingTight,
        encodingZRLE,
        encodingHextile,
        encodingRRE,
        encodingRaw,
        pseudoEncodingCompressLevel9,
        pseudoEncodingQualityLevel9,
        pseudoEncodingFineQualityLevel100,
        pseudoEncodingSubsamp16X
        //pseudoEncodingWEBP
        //pseudoEncodingQOI
    };

    class MockCConnection final : public CConnection {
    public:
        explicit MockCConnection(ManagedPixelBuffer *pb) {
            setStreams(&in, nullptr);

            // Need to skip the initial handshake and ServerInit
            setState(RFBSTATE_NORMAL);
            // That also means that the reader and writer weren't set up
            setReader(new rfb::CMsgReader(this, &in));
            auto &pf = pb->getPF();
            CMsgHandler::setPixelFormat(pf);

            MockCConnection::setDesktopSize(pb->width(), pb->height());

            cp.setPF(pf);

            sc.cp.setPF(pf);
            sc.setEncodings(std::size(encodings), encodings);

            setFramebuffer(pb);
        }

        void setCursor(int width, int height, const Point &hotspot, const rdr::U8 *data, const bool resizing) override {
        }

        ~MockCConnection() override = default;


        struct stats_t {
            EncodeManager::codecstats_t jpeg_stats;
            EncodeManager::codecstats_t webp_stats;
            uint64_t bytes;
            uint64_t udp_bytes;
        };

        [[nodiscard]] stats_t getStats() const {
            return {
                sc.getJpegStats(),
                sc.getWebPStats(),
                sc.bytes,
                sc.udp_bytes
            };
        }

        void setDesktopSize(int w, int h) override {
            CConnection::setDesktopSize(w, h);

            if (screen_layout.num_screens())
                screen_layout.remove_screen(0);

            screen_layout.add_screen(Screen(0, 0, 0, w, h, 0));
        }

        void setNewFrame(const AVFrame *frame) {
            auto *pb = getFramebuffer();
            const int width = pb->width();
            const int height = pb->height();
            const rfb::Rect rect(0, 0, width, height);

            int dstStride{};
            auto *buffer = pb->getBufferRW(rect, &dstStride);

            const PixelFormat &pf = pb->getPF();

            // Source data and stride from FFmpeg
            const auto *srcData = frame->data[0];
            const int srcStride = frame->linesize[0] / 3; // Convert bytes to pixels

            // Convert from the RGB format to the PixelBuffer's format
            pf.bufferFromRGB(buffer, srcData, width, srcStride, height);

            // Commit changes
            pb->commitBufferRW(rect);
        }

        void framebufferUpdateStart() override {
            CConnection::framebufferUpdateStart();

            updates.clear();
        }

        void framebufferUpdateEnd() override {
            const PixelBuffer *pb = getFramebuffer();

            UpdateInfo ui;

            const Region clip(pb->getRect());

            updates.add_changed(pb->getRect());

            updates.getUpdateInfo(&ui, clip);
            sc.writeUpdate(ui, pb);
        }

        void dataRect(const Rect &r, int encoding) override {
            CConnection::dataRect(r, encoding);

            if (encoding != encodingCopyRect) // FIXME
                updates.add_changed(Region(r));
        }

        void setColourMapEntries(int, int, rdr::U16 *) override {
        }

        void bell() override {
        }

        void serverCutText(const char *, rdr::U32) override {
        }

        void serverCutText(const char *str) override {
        }

    protected:
        MockBufferStream in;
        ScreenSet screen_layout;
        SimpleUpdateTracker updates;
        MockSConnection sc;
    };
}

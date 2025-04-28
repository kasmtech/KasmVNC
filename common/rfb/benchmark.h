/* Copyright 2015 Pierre Ossman <ossman@cendio.se> for Cendio AB
 * Copyright (C) 2015 D. R. Commander.  All Rights Reserved.
 * Copyright (C) 2025 Kasm Technologies Corp
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

#include <cassert>
#include <rdr/FileInStream.h>
#include <rfb/VNCServer.h>

#include "CConnection.h"
#include "CMsgReader.h"
#include "EncCache.h"
#include "EncodeManager.h"
#include "LogWriter.h"
#include "screenTypes.h"
#include "SMsgWriter.h"
#include "ffmpeg.h"

static rfb::LogWriter vlog("Benchmarking");

namespace rfb {
    static constexpr rdr::S32 default_encodings[] = {
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

    class MockBufferStream final : public rdr::BufferedInStream {
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
                cache.clear();

            manager.clearEncodingTime();
            if (!ui.is_empty()) {
                manager.writeUpdate(ui, pb, nullptr);
            } else {
                Region region{pb->getRect()};
                manager.writeLosslessRefresh(region, pb, nullptr, 2000);
            }
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

        [[nodiscard]] auto bytes() { return out.length(); }
        [[nodiscard]] auto udp_bytes() { return udps.length(); }

    protected:
        MockStream out{};
        MockStream udps{};

        EncCache cache{};
        EncodeManager manager{this, &cache};
    };

    class MockCConnection final : public CConnection {
    public:
        explicit MockCConnection(const std::vector<rdr::S32>& encodings, ManagedPixelBuffer *pb) {
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
            sc.setEncodings(std::size(encodings), encodings.data());

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

        [[nodiscard]] stats_t getStats() {
            return {
                sc.getJpegStats(),
                sc.getWebPStats(),
                sc.bytes(),
                sc.udp_bytes()
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

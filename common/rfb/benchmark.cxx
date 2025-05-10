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

#include "benchmark.h"
#include <string_view>
#include <rfb/LogWriter.h>
#include <numeric>
#include <tinyxml2.h>
#include <algorithm>
#include <cassert>

#include "ServerCore.h"
#include <cmath>

#include "EncCache.h"
#include "EncodeManager.h"
#include "SConnection.h"
#include "screenTypes.h"
#include "SMsgWriter.h"
#include "UpdateTracker.h"
#include "rdr/BufferedInStream.h"
#include "rdr/OutStream.h"
#include "ffmpeg.h"

namespace benchmarking {
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

    class MockSConnection final : public rfb::SConnection {
    public:
        MockSConnection() {
            setStreams(nullptr, &out);

            setWriter(new rfb::SMsgWriter(&cp, &out, &udps));
        }

        ~MockSConnection() override = default;

        void writeUpdate(const rfb::UpdateInfo &ui, const rfb::PixelBuffer *pb) {
            cache.clear();

            manager.clearEncodingTime();
            if (!ui.is_empty()) {
                manager.writeUpdate(ui, pb, nullptr);
            } else {
                rfb::Region region{pb->getRect()};
                manager.writeLosslessRefresh(region, pb, nullptr, 2000);
            }
        }

        void setDesktopSize(int fb_width, int fb_height,
                            const rfb::ScreenSet &layout) override {
            cp.width = fb_width;
            cp.height = fb_height;
            cp.screenLayout = layout;

            writer()->writeExtendedDesktopSize(rfb::reasonServer, 0, cp.width, cp.height,
                                               cp.screenLayout);
        }

        void sendStats(const bool toClient) override {
        }

        [[nodiscard]] bool canChangeKasmSettings() const override {
            return true;
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

    class MockCConnection final : public MockTestConnection {
    public:
        explicit MockCConnection(const std::vector<rdr::S32> &encodings, rfb::ManagedPixelBuffer *pb) {
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

        void setCursor(int width, int height, const rfb::Point &hotspot, const rdr::U8 *data,
                       const bool resizing) override {
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

            screen_layout.add_screen(rfb::Screen(0, 0, 0, w, h, 0));
        }

        void setNewFrame(const AVFrame *frame) override {
            auto *pb = getFramebuffer();
            const int width = pb->width();
            const int height = pb->height();
            const rfb::Rect rect(0, 0, width, height);

            int dstStride{};
            auto *buffer = pb->getBufferRW(rect, &dstStride);

            const rfb::PixelFormat &pf = pb->getPF();

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
            const rfb::PixelBuffer *pb = getFramebuffer();

            rfb::UpdateInfo ui;
            const rfb::Region clip(pb->getRect());

            updates.add_changed(pb->getRect());

            updates.getUpdateInfo(&ui, clip);
            sc.writeUpdate(ui, pb);
        }

        void dataRect(const rfb::Rect &r, int encoding) override {
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
        rfb::ScreenSet screen_layout;
        rfb::SimpleUpdateTracker updates;
        MockSConnection sc;
    };
}

void report(std::vector<uint64_t> &totals, std::vector<uint64_t> &timings,
            std::vector<benchmarking::MockCConnection::stats_t> &stats, const std::string_view results_file) {
    auto totals_sum = std::accumulate(totals.begin(), totals.end(), 0.);
    auto totals_avg = totals_sum / static_cast<double>(totals.size());

    auto variance = 0.;
    for (auto t: totals)
        variance += (static_cast<double>(t) - totals_avg) * (static_cast<double>(t) - totals_avg);

    variance /= static_cast<double>(totals.size());
    auto stddev = std::sqrt(variance);

    const auto sum = std::accumulate(timings.begin(), timings.end(), 0.);
    const auto size = timings.size();
    const auto average = sum / static_cast<double>(size);

    double median{};

    std::sort(timings.begin(), timings.end());
    if (size % 2 == 0)
        median = static_cast<double>(timings[size / 2]);
    else
        median = static_cast<double>(timings[size / 2 - 1] + timings[size / 2]) / 2.;

    vlog.info("Mean time encoding frame: %f ms", average);
    vlog.info("Median time encoding frame: %f ms", median);
    vlog.info("Total time (mean): %f ms", totals_avg);
    vlog.info("Total time (stddev): %f ms", stddev);

    uint32_t jpeg_sum{}, jpeg_rects{}, webp_sum{}, webp_rects{};
    uint64_t bytes{};

    for (const auto &item: stats) {
        jpeg_sum += item.jpeg_stats.ms;
        jpeg_rects += item.jpeg_stats.rects;
        webp_sum += item.webp_stats.ms;
        webp_rects += item.webp_stats.rects;
        bytes += item.bytes;
    }

    auto jpeg_ms = jpeg_sum / static_cast<double>(stats.size());
    vlog.info("JPEG stats: %f ms", jpeg_ms);
    jpeg_rects /= stats.size();
    vlog.info("JPEG stats: %u rects", jpeg_rects);
    auto webp_ms = webp_sum / static_cast<double>(stats.size());
    webp_rects /= stats.size();
    bytes /= stats.size();
    vlog.info("WebP stats: %f ms", webp_ms);
    vlog.info("WebP stats: %u rects", webp_rects);
    vlog.info("Total bytes sent: %lu bytes", bytes);

    tinyxml2::XMLDocument doc;

    auto *test_suit = doc.NewElement("testsuite");
    test_suit->SetAttribute("name", "Benchmark");

    doc.InsertFirstChild(test_suit);
    auto total_tests{0};

    auto add_benchmark_item = [&doc, &test_suit, &total_tests](const char *name, auto time_value, auto other_value) {
        auto *test_case = doc.NewElement("testcase");
        test_case->SetAttribute("name", name);
        test_case->SetAttribute("file", other_value);
        test_case->SetAttribute("time", time_value);
        test_case->SetAttribute("runs", 1);
        test_case->SetAttribute("classname", "KasmVNC");

        test_suit->InsertEndChild(test_case);

        ++total_tests;
    };

    constexpr auto mult = 1 / 1000.;
    add_benchmark_item("Average time encoding frame, ms", average * mult, "");
    add_benchmark_item("Median time encoding frame, ms", median * mult, "");
    add_benchmark_item("Total time encoding, ms", 0, totals_avg);
    add_benchmark_item("Total time encoding, stddev", 0, stddev);
    add_benchmark_item("Mean JPEG stats, ms", jpeg_ms, "");
    add_benchmark_item("Mean JPEG stats, rects", 0., jpeg_rects);
    add_benchmark_item("Mean WebP stats, ms", webp_ms, "");
    add_benchmark_item("Mean WebP stats, rects", 0, webp_rects);

    add_benchmark_item("Data sent, KBs", 0, bytes / 1024);

    doc.SaveFile(results_file.data());
}

void benchmark(std::string_view path, const std::string_view results_file) {
    try {
        vlog.info("Benchmarking with video file %s", path.data());
        FFmpegFrameFeeder frame_feeder{};
        frame_feeder.open(path);

        static const rfb::PixelFormat pf{32, 24, false, true, 0xFF, 0xFF, 0xFF, 0, 8, 16};
        std::vector<rdr::S32> encodings{
            std::begin(benchmarking::default_encodings), std::end(benchmarking::default_encodings)
        };

        constexpr auto runs = 20;
        std::vector<uint64_t> totals(runs, 0);
        std::vector<benchmarking::MockCConnection::stats_t> stats(runs);
        std::vector<uint64_t> timings{};
        auto [width, height] = frame_feeder.get_frame_dimensions();

        for (int run = 0; run < runs; ++run) {
            auto *pb = new rfb::ManagedPixelBuffer{pf, width, height};
            benchmarking::MockCConnection connection{encodings, pb};

            vlog.info("RUN %d. Reading frames...", run);
            auto play_stats = frame_feeder.play(&connection);
            vlog.info("RUN %d. Done reading frames...", run);

            timings.insert(timings.end(), play_stats.timings.begin(), play_stats.timings.end());

            totals[run] = play_stats.total;
            stats[run] = connection.getStats();
            vlog.info("JPEG stats: %u ms", stats[run].jpeg_stats.ms);
            vlog.info("WebP stats: %u ms", stats[run].webp_stats.ms);
            vlog.info("RUN %d. Bytes sent %lu..", run, stats[run].bytes);
        }

        if (!timings.empty())
            report(totals, timings, stats, results_file);

        exit(0);
    } catch (std::exception &e) {
        vlog.error("Benchmarking failed: %s", e.what());
        exit(1);
    }
}

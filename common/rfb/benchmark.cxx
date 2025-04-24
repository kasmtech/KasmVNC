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

#include "benchmark.h"
#include <string>
#include <stdexcept>
#include <rfb/LogWriter.h>
#include <numeric>
#include <tinyxml2.h>
#include <algorithm>
#include "ServerCore.h"

void report(std::vector<uint64_t> &totals, std::vector<uint64_t> &timings,
            std::vector<rfb::MockCConnection::stats_t> &stats, const std::string &results_file) {
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

    doc.SaveFile(results_file.c_str());
}

void benchmark(const std::string &path, const std::string &results_file) {
    AVFormatContext *format_ctx = nullptr;

    vlog.info("Benchmarking with video file %s", path.c_str());

    if (avformat_open_input(&format_ctx, path.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("Could not open video file");

    FormatCtxGuard format_ctx_guard{format_ctx};

    // Find stream info
    if (avformat_find_stream_info(format_ctx, nullptr) < 0)
        throw std::runtime_error("Could not find stream info");

    // Find video stream
    int video_stream_idx = -1;
    for (uint32_t i = 0; i < format_ctx->nb_streams; ++i) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_idx == -1)
        throw std::runtime_error("No video stream found");

    // Get codec parameters and decoder
    const auto *codec_parameters = format_ctx->streams[video_stream_idx]->codecpar;
    const auto *codec = avcodec_find_decoder(codec_parameters->codec_id);
    if (!codec)
        throw std::runtime_error("Codec not found");

    const CodecCtxGuard codex_ctx_guard{avcodec_alloc_context3(codec)};
    auto *codec_ctx = codex_ctx_guard.get();

    if (!codec_ctx || avcodec_parameters_to_context(codec_ctx, codec_parameters) < 0)
        throw std::runtime_error("Failed to set up codec context");

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0)
        throw std::runtime_error("Could not open codec");

    // Allocate frame and packet
    const FrameGuard frame_guard{av_frame_alloc()};
    auto *frame = frame_guard.get();

    const PacketGuard packet_guard{av_packet_alloc()};
    auto *packet = packet_guard.get();

    if (!frame || !packet)
        throw std::runtime_error("Could not allocate frame or packet");

    // Scaling context to convert to RGB24
    SwsContext *sws_ctx = sws_getContext(
        codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
        codec_ctx->width, codec_ctx->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx)
        throw std::runtime_error("Could not create scaling context");

    SwsContextGuard sws_ctx_guard{sws_ctx};

    const FrameGuard rgb_frame_guard{av_frame_alloc()};
    auto *rgb_frame = rgb_frame_guard.get();

    if (!rgb_frame)
        throw std::runtime_error("Could not allocate frame");

    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width = codec_ctx->width;
    rgb_frame->height = codec_ctx->height;

    static const rfb::PixelFormat pf{32, 24, false, true, 0xFF, 0xFF, 0xFF, 0, 8, 16};
    std::vector<rdr::S32> encodings{std::begin(rfb::default_encodings), std::end(rfb::default_encodings)};

    // if (rfb::Server::WebPEnabled)
    //     encodings.push_back(rfb::pseudoEncodingWEBP);

    if (av_frame_get_buffer(rgb_frame, 0) != 0)
        throw std::runtime_error("Could not allocate frame data");

    constexpr auto runs = 20;
    std::vector<uint64_t> totals(runs, 0);
    std::vector<rfb::MockCConnection::stats_t> stats(runs);
    const size_t total_frame_count = format_ctx->streams[video_stream_idx]->nb_frames;
    std::vector<uint64_t> timings(total_frame_count > 0 ? total_frame_count * runs : 2048, 0);
    uint64_t frames{};

    for (int run = 0; run < runs; ++run) {
        auto *pb = new rfb::ManagedPixelBuffer{pf, rgb_frame->width, rgb_frame->height};
        rfb::MockCConnection connection{encodings, pb};

        uint64_t total{};

        vlog.info("RUN %d. Reading frames...", run);
        while (av_read_frame(format_ctx, packet) == 0) {
            if (packet->stream_index == video_stream_idx) {
                if (avcodec_send_packet(codec_ctx, packet) == 0) {
                    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                        // Convert to RGB
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
                                  rgb_frame->data, rgb_frame->linesize);

                        connection.framebufferUpdateStart();
                        connection.setNewFrame(rgb_frame);
                        using namespace std::chrono;

                        auto now = high_resolution_clock::now();
                        connection.framebufferUpdateEnd();
                        const auto duration = duration_cast<milliseconds>(high_resolution_clock::now() - now).count();

                        //vlog.info("Frame took %lu ms", duration);

                        timings[frames++] = duration;
                        total += duration;
                    }
                }
            }
            av_packet_unref(packet);
        }
        vlog.info("RUN %d. Done reading frames...", run);

        if (av_seek_frame(format_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD) < 0)
            throw std::runtime_error("Could not seek to start of video");

        avcodec_flush_buffers(codec_ctx);

        totals[run] = total;
        stats[run] = connection.getStats();
        vlog.info("RUN %d. Bytes sent %lu..", run, stats[run].bytes);
    }

    if (frames > 0)
        report(totals, timings, stats, results_file);

    avcodec_close(codec_ctx);

    exit(0);
}

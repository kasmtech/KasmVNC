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

#include "benchmark.h"
#include <string>
#include <stdexcept>
#include <rfb/LogWriter.h>
#include <numeric>
#include <tinyxml2.h>
#include <algorithm>
#include <sstream>

void benchmark(const std::string &path) {
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

    auto *pb = new rfb::ManagedPixelBuffer{pf, rgb_frame->width, rgb_frame->height};
    rfb::MockCConnection connection{pb};

    if (av_frame_get_buffer(rgb_frame, 0) != 0)
        throw std::runtime_error("Could not allocate frame data");

    uint64_t frames{};
    const size_t total_frame_count = format_ctx->streams[video_stream_idx]->nb_frames;

    std::vector<uint64_t> timings(total_frame_count > 0 ? total_frame_count : 2048, 0);

    vlog.info("Reading frames...");
    while (av_read_frame(format_ctx, packet) == 0) {
        if (packet->stream_index == video_stream_idx) {
            if (avcodec_send_packet(codec_ctx, packet) == 0) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    // Convert to RGB
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height,
                              rgb_frame->data, rgb_frame->linesize);

                    vlog.info("Updating with frame %lu", frames);
                    connection.framebufferUpdateStart();
                    vlog.info("Setting frame");
                    connection.setNewFrame(rgb_frame);
                    using namespace std::chrono;

                    auto now = high_resolution_clock::now();
                    vlog.info("Updating frame...");
                    connection.framebufferUpdateEnd();
                    const auto duration = duration_cast<nanoseconds>(high_resolution_clock::now() - now).count();

                    vlog.info("Frame took %lu ns", duration);

                    auto [jpeg_stats, webp_stats, bytes, udp_bytes] = connection.getStats();
                    //vlog.info("JPEG stats: %d ms", jpeg_stats.ms);
                    //vlog.info("JPEG stats: %d rects", jpeg_stats.rects);

                    //vlog.info("WebP stats: %d ms", webp_stats.ms);
                    // vlog.info("WebP stats: %d rects", webp_stats.rects);

                    timings[frames++] = duration;
                }
            }
        }
        av_packet_unref(packet);
    }
    vlog.info("Done reading frames...");

    if (frames > 0) {
        timings.reserve(frames + 1);

        const auto sum = std::accumulate(timings.begin(), timings.end(), 0.);
        const auto size = timings.size();
        const auto average = sum / static_cast<double>(size);

        double median{};

        std::sort(timings.begin(), timings.end());
        if (size % 2 == 0)
            median = static_cast<double>(timings[size / 2]);
        else
            median = static_cast<double>(timings[size / 2 - 1] + timings[size / 2]) / 2.;

        auto [jpeg_stats, webp_stats, bytes, udp_bytes] = connection.getStats();

        vlog.info("Average time encoding frame: %f ns", average);
        vlog.info("Median time encoding frame: %f ns", median);
        vlog.info("Total time: %f ns", sum);

        tinyxml2::XMLDocument doc;

        auto *test_suit = doc.NewElement("testsuite");
        test_suit->SetAttribute("name", "Benchmark");

        doc.InsertFirstChild(test_suit);

        constexpr auto div = 1. / (1000 * 1000);
        auto total_tests{0};

        auto add_benchmark_item = [&doc, &test_suit, &total_tests](const char *name, auto value) {
            auto *test_case = doc.NewElement("testcase");
            test_case->SetAttribute("name", name);
            test_case->SetAttribute("time", value);
            test_case->SetAttribute("runs", 1);
            test_case->SetAttribute("classname", "KasmVNC");
            test_suit->InsertEndChild(test_case);

            ++total_tests;
        };

        add_benchmark_item("Average time encoding frame, ms", average * div);
        add_benchmark_item("Median time encoding frame, ms", median * div);
        add_benchmark_item("Total time encoding, ms", sum * div);

        std::stringstream ss;
        ss << "KBytes sent: " << bytes / 1024;
        add_benchmark_item(ss.str().c_str(), 0);

        //ss.flush();
        //ss << "KBytes sent (UDP): " << udp_bytes / 1024;
        //add_benchmark_item(ss.str().c_str(), 0);

        doc.SaveFile("Benchmark.xml");
    }

    exit(0);
}

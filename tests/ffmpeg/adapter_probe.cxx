/* Copyright (C) 2026 Kasm Technologies Corp. GPL-2.0-or-later. */
#include "rfb/ffmpeg.h"
#include "rfb/LogWriter.h"
#include "rfb/Logger_stdio.h"
#include <fmt/format.h>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static void check_hardware_options(const FFmpeg &ff) {
    struct Candidate {
        const char *name;
        FFmpeg::Device device;
        int low, high, profile;
    };
    const Candidate candidates[] = {
        {"h264_vaapi", FFmpeg::Device::VAAPI, 0, 50, 77},
        {"hevc_vaapi", FFmpeg::Device::VAAPI, 0, 50, 1},
        {"av1_vaapi", FFmpeg::Device::VAAPI, 1, 62, 0},
        {"h264_nvenc", FFmpeg::Device::CUDA, 0, 50, 77},
        {"hevc_nvenc", FFmpeg::Device::CUDA, 0, 50, 1},
        {"av1_nvenc", FFmpeg::Device::CUDA, 19, 62, 0},
    };
    for (const auto &candidate: candidates) {
        for (const int quality: {candidate.low, 25, candidate.high}) {
            FFmpeg::EncoderSettings settings{
                candidate.name, candidate.device,
                candidate.device == FFmpeg::Device::VAAPI ? "/kasm-test-missing-device" : "2147483647",
                96, 64, 30, 30, quality, candidate.profile, candidate.high
            };
            if (!ff.has_encoder(candidate.name)) {
                // FFmpeg 4.4/5.1 predate both AV1 hardware encoders. Distros may
                // backport one, so exercise it if present instead of skipping by
                // family alone. Missing H.264/HEVC remains a test failure.
                if (ff.version_major() < 60 && std::string(candidate.name).starts_with("av1_")) {
                    std::cout << "SKIP unavailable on this runtime: " << candidate.name << '\n';
                    break;
                }
                throw std::runtime_error(fmt::format("{} is not available", candidate.name));
            }
            const auto result = ff.probe(settings);
            if (result.category == FFmpeg::ProbeCategory::Ok)
                throw std::runtime_error(fmt::format(
                    "Unexpectedly created the deliberately invalid hardware device: {} device={} quality={} profile={}",
                    candidate.name, settings.device_path, quality, candidate.profile));
            if (result.category == FFmpeg::ProbeCategory::Option)
                throw std::runtime_error(fmt::format("{} quality={}: encoder rejected an option", candidate.name,
                                                     quality));
            if (result.category != FFmpeg::ProbeCategory::Hardware)
                throw std::runtime_error(
                    fmt::format("{} quality={}: expected device failure, got: {}", candidate.name, quality,
                                result.message));
            std::cout << "PASS options " << candidate.name << " quality=" << quality << '\n';
        }
    }
}

static void encode(const FFmpeg &ff, const std::string &path, const char *name, int width, int height) {
    FFmpeg::EncoderSettings settings{
        name, FFmpeg::Device::Software, "", width, height, 30, 30, 25,
        std::string(name) == "libx264" ? (66 | 1 << 9) : 1, 50
    };
    require(ff.probe(settings).category != FFmpeg::ProbeCategory::Option, "encoder rejected an option");
    const auto encoder = ff.create_encoder(settings);
    require(bool(encoder), "cannot create encoder");
    std::ofstream output(path, std::ios::binary);
    require(bool(output), "cannot open encoded output");
    std::vector<uint8_t> pixels(width * height * 4);
    unsigned count = 0, keys = 0;
    int64_t previous = -1;
    for (int frame = 0; frame < 24; ++frame) {
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i + frame * 13);
        require(encoder->next_pts() == frame, "encoder PTS counter out of sequence");
        require(encoder->encode_argb(pixels.data(), width * 4, frame == 8 || frame == 16), "encode frame");
        for (;;) {
            const auto packet = encoder->packet();
            if (!packet.data) break;
            require(packet.size > 0 && packet.pts > previous, "packet size or PTS ordering");
            require(packet.pts < 24, "packet PTS out of range");
            if (packet.pts == 8 || packet.pts == 16) require(packet.keyframe, "forced keyframe not honored");
            previous = packet.pts;
            keys += packet.keyframe;
            ++count;
            output.write(reinterpret_cast<const char *>(packet.data), packet.size);
            encoder->pop_packet();
        }
    }
    require(count == 24 && keys >= 3, "low-delay frame or keyframe count mismatch");
    require(bool(output), "failed to write encoded output");
}

int main(int argc, char **argv) {
    rfb::initStdIOLoggers();
    rfb::LogWriter::setLogParams("*:stderr:100");
    try {
        const auto &ff = FFmpeg::get();
        if (argc > 1 && std::string(argv[1]) == "--expect-unavailable") {
            require(!ff.is_available(), "unexpected available FFmpeg runtime");
            std::cout << "PASS unavailable runtime\n";
            return 0;
        }
        require(ff.is_available(), "FFmpeg runtime unavailable");
        require(argc >= 3, "usage: ffmpeg-adapter-probe output-prefix expected-major [libx264|libx265]");
        require(ff.version_major() == unsigned(std::stoi(argv[2])), "wrong selected ABI family");
        if (std::string(argv[1]) == "--check-hardware-options") {
            check_hardware_options(ff);
            std::cout << "PASS hardware option boundaries " << ff.description() << '\n';
            return 0;
        }
        const char *name = argc > 3 ? argv[3] : "libx264";
        const std::string prefix = argv[1];
        auto second = std::async(std::launch::async, [&] { encode(ff, prefix + "-second.bin", name, 128, 96); });
        encode(ff, prefix + ".bin", name, 96, 64);
        second.get();
        // A new size uses a new context, matching the screen reconfiguration path.
        encode(ff, prefix + "-resized.bin", name, 160, 96);
        auto source = ff.open_source(prefix + ".bin");
        for (int pass = 0; pass < 2; ++pass) {
            FFmpeg::RGBFrame frame;
            unsigned count = 0;
            while (source->next(frame)) {
                require(frame.width == 96 && frame.height == 64 && frame.stride >= 96 * 3, "decoded dimensions/stride");
                ++count;
            }
            require(count == 24, "benchmark decode/drain frame count");
            source->rewind();
        }
        auto absent = FFmpeg::EncoderSettings{"kasm_nonexistent_encoder", FFmpeg::Device::Software, "", 96, 64};
        require(!ff.probe_encoder(absent), "missing encoder advertised");
        std::cout << "PASS " << ff.description() << " " << name << " concurrent/resize/keyframe/decode/seek\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}

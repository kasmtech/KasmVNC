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

#include "ffmpeg.h"
#include "ffmpeg/Backend.h"
#include "LogWriter.h"
#include <cstdlib>
#include <stdexcept>

#ifdef KASMVNC_FFMPEG_ADAPTERS
FFmpegBackend *make_ffmpeg_58();
FFmpegBackend *make_ffmpeg_59();
FFmpegBackend *make_ffmpeg_60();
FFmpegBackend *make_ffmpeg_61();
FFmpegBackend *make_ffmpeg_62();
FFmpegBackend *make_ffmpeg_63();
#else
FFmpegBackend *make_ffmpeg_legacy();
#endif

static rfb::LogWriter vlog("ffmpeg");

FFmpeg &FFmpeg::get() {
    static FFmpeg instance;
    return instance;
}

FFmpeg::FFmpeg() {
#ifdef KASMVNC_FFMPEG_ADAPTERS
    // A diagnostic override is useful when several supported families coexist.
    const char *requested = std::getenv("KASMVNC_FFMPEG_MAJOR");
    const std::string selected = requested ? requested : "";
    const struct {
        const char *major;

        FFmpegBackend *(*create)();
    } adapters[] = {
        {"63", make_ffmpeg_63},
        {"62", make_ffmpeg_62},
        {"61", make_ffmpeg_61},
        {"60", make_ffmpeg_60},
        {"59", make_ffmpeg_59},
        {"58", make_ffmpeg_58},
    };
    bool supported = selected.empty();
    // Prefer the newest available family. A diagnostic override selects exactly
    // one family, so a missing requested runtime never silently uses another.
    for (const auto &adapter: adapters) {
        if (!selected.empty() && selected != adapter.major)
            continue;

        supported = true;
        backend = adapter.create();
        if (backend)
            break;
    }
    if (!supported) {
        vlog.error("Unsupported KASMVNC_FFMPEG_MAJOR=%s (supported: 58, 59, 60, 61, 62, 63)", selected.c_str());
        return;
    }
#else
    backend = make_ffmpeg_legacy();
#endif
    if (backend)
        vlog.info("Selected %s", backend->description().c_str());
    else
        vlog.error("No supported FFmpeg runtime is available; video encoders disabled (see -Log ffmpeg:stdout:100)");
}

FFmpeg::~FFmpeg() = default;

bool FFmpeg::is_available() const noexcept { return backend; }
unsigned FFmpeg::version_major() const noexcept { return backend ? backend->major() : 0; }
std::string FFmpeg::description() const { return backend ? backend->description() : "unavailable"; }

std::unique_ptr<FFmpeg::Encoder> FFmpeg::create_encoder(const EncoderSettings &settings) const noexcept {
    try {
        if (!backend)
            throw std::runtime_error("FFmpeg is not available");
        return backend->create_encoder(settings);
    } catch (const std::exception &e) {
        vlog.debug("Encoder %s unavailable: %s", settings.name.c_str(), e.what());
    } catch (...) {
        vlog.debug("Encoder %s unavailable: unknown error", settings.name.c_str());
    }
    return nullptr;
}

bool FFmpeg::has_encoder(const std::string &name) const {
    return backend && backend->has_encoder(name);
}

bool FFmpeg::probe_encoder(const EncoderSettings &settings) const {
    const auto category = probe(settings).category;
    return category == ProbeCategory::Ok || category == ProbeCategory::Option;
}

static FFmpeg::ProbeCategory failure_category(FFmpeg::ProbeCategory last_error) {
    return last_error == FFmpeg::ProbeCategory::Ok ? FFmpeg::ProbeCategory::Error : last_error;
}

FFmpeg::ProbeResult FFmpeg::probe(const EncoderSettings &settings) const {
    if (!backend)
        return {ProbeCategory::Error, "FFmpeg is not available"};
    try {
        if (backend->create_encoder(settings)) {
            if (backend->get_last_error() == ProbeCategory::Option)
                return {ProbeCategory::Option, "encoder rejected an option"};
            return {};
        }
        return {ProbeCategory::Error, "no encoder returned"};
    } catch (const std::exception &e) {
        vlog.debug("Encoder %s unavailable: %s", settings.name.c_str(), e.what());
        return {failure_category(backend->get_last_error()), e.what()};
    } catch (...) {
        vlog.debug("Encoder %s unavailable: unknown error", settings.name.c_str());
        return {failure_category(backend->get_last_error()), "unknown error"};
    }
}

std::unique_ptr<FFmpeg::FrameSource> FFmpeg::open_source(const std::string &path) const {
    if (!backend)
        throw std::runtime_error("FFmpeg is not available");

    return backend->open_source(path);
}

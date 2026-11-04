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

#include <config.h>
#include "ffmpeg.h"
#include "ffmpeg/Backend.h"
#include "LogWriter.h"
#include <cstdlib>
#include <stdexcept>

#ifdef KASMVNC_FFMPEG_ADAPTERS
std::unique_ptr<FFmpegBackend> make_ffmpeg_60();
std::unique_ptr<FFmpegBackend> make_ffmpeg_61();
#else
std::unique_ptr<FFmpegBackend> make_ffmpeg_legacy();
#endif

static rfb::LogWriter vlog("ffmpeg");
FFmpeg &FFmpeg::get() { static FFmpeg instance; return instance; }
FFmpeg::FFmpeg() {
#ifdef KASMVNC_FFMPEG_ADAPTERS
    // A diagnostic override is useful when several supported families coexist.
    const char *requested = std::getenv("KASMVNC_FFMPEG_MAJOR");
    const std::string selected = requested ? requested : "";
    if (!selected.empty() && selected != "60" && selected != "61") {
        vlog.error("Unsupported KASMVNC_FFMPEG_MAJOR=%s (supported: 60, 61)", selected.c_str());
        return;
    }
    if (selected.empty() || selected == "61") backend = make_ffmpeg_61();
    if (!backend && (selected.empty() || selected == "60")) backend = make_ffmpeg_60();
#else
    backend = make_ffmpeg_legacy();
#endif
    if (backend) vlog.info("Selected %s", backend->description().c_str());
    else vlog.error("No supported FFmpeg runtime is available; video encoders disabled");
}
FFmpeg::~FFmpeg() = default;
bool FFmpeg::is_available() const noexcept { return bool(backend); }
unsigned FFmpeg::version_major() const noexcept { return backend ? backend->major() : 0; }
std::string FFmpeg::description() const { return backend ? backend->description() : "unavailable"; }
std::unique_ptr<FFmpeg::Encoder> FFmpeg::create_encoder(const EncoderSettings &settings) const {
    if (!backend) throw std::runtime_error("FFmpeg is not available");
    return backend->create_encoder(settings);
}
bool FFmpeg::probe_encoder(const EncoderSettings &settings) const {
    try { return bool(create_encoder(settings)); }
    catch (const std::exception &e) {
        vlog.debug("Encoder %s unavailable: %s", settings.name.c_str(), e.what());
        return false;
    }
}
std::unique_ptr<FFmpeg::FrameSource> FFmpeg::open_source(const std::string &path) const {
    if (!backend) throw std::runtime_error("FFmpeg is not available");
    return backend->open_source(path);
}

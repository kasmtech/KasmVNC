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

#include <rfb/ffmpeg.h>

class FFmpegBackend {
public:
    virtual ~FFmpegBackend() = default;
    [[nodiscard]] virtual unsigned major() const = 0;
    [[nodiscard]] virtual std::string description() const = 0;
    [[nodiscard]] virtual bool has_encoder(const std::string &name) const = 0;
    virtual std::unique_ptr<FFmpeg::Encoder> create_encoder(const FFmpeg::EncoderSettings &) = 0;
    virtual std::unique_ptr<FFmpeg::FrameSource> open_source(const std::string &) = 0;
};

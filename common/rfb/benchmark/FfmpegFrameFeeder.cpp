/* Copyright (C) 2025 Kasm.  All Rights Reserved.
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
#include "FfmpegFrameFeeder.h"
#include <chrono>
FfmpegFrameFeeder::FfmpegFrameFeeder(FFmpeg *ffmpeg_) : ffmpeg(ffmpeg_) {}
FfmpegFrameFeeder::~FfmpegFrameFeeder() = default;
void FfmpegFrameFeeder::open(std::string_view path) { source = ffmpeg->open_source(std::string(path)); }
FfmpegFrameFeeder::play_stats_t FfmpegFrameFeeder::play(benchmarking::MockTestConnection *connection) const {
    play_stats_t stats{};
    const auto count = get_total_frame_count();
    stats.timings.reserve(count > 0 ? count : 2048);
    FFmpeg::RGBFrame frame;
    while (source->next(frame)) {
        connection->framebufferUpdateStart();
        connection->setNewFrame(&frame);
        const auto start = std::chrono::steady_clock::now();
        connection->framebufferUpdateEnd();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        ++stats.frames;
        stats.total += elapsed;
        stats.timings.push_back(elapsed);
    }
    source->rewind();
    return stats;
}

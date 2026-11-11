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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// This boundary contains no FFmpeg types. All objects and their allocations stay
// inside the implementation compiled against the selected library's headers.
class FFmpegBackend;

class FFmpeg final {
public:
    enum class Device { Software, VAAPI, CUDA };

    struct EncoderSettings {
        std::string name;
        Device device{Device::Software};
        std::string device_path;
        int width{}, height{}, frame_rate{60}, gop{30}, quality{25};
        int profile{}, maximum_quality{50};
        int64_t first_pts{};
    };

    struct Packet {
        const uint8_t *data{};
        size_t size{};
        bool keyframe{};
        int64_t pts{};
    };

    class Encoder {
    public:
        virtual ~Encoder() = default;

        virtual void encode_argb(const uint8_t *data, int stride, bool keyframe) = 0;
        [[nodiscard]] virtual int64_t next_pts() const = 0;
        [[nodiscard]] virtual Packet packet() const = 0;
        virtual void pop_packet() = 0;
    };

    struct RGBFrame {
        const uint8_t *data{};
        int stride{}, width{}, height{};
    };

    class FrameSource {
    public:
        virtual ~FrameSource() = default;

        [[nodiscard]] virtual int width() const = 0;
        [[nodiscard]] virtual int height() const = 0;
        [[nodiscard]] virtual int64_t frame_count() const = 0;

        // The returned RGB pixels remain valid until the next next()/rewind().
        virtual bool next(RGBFrame &frame) = 0;
        virtual void rewind() = 0;
    };

    FFmpeg(const FFmpeg &) = delete;
    FFmpeg &operator=(const FFmpeg &) = delete;

    static FFmpeg &get();
    [[nodiscard]] bool is_available() const noexcept;
    [[nodiscard]] unsigned version_major() const noexcept;
    [[nodiscard]] std::string description() const;

    // Throws if the encoder cannot be created; never returns null.
    [[nodiscard]] std::unique_ptr<Encoder> create_encoder(const EncoderSettings &settings) const;
    [[nodiscard]] bool has_encoder(const std::string &name) const;
    [[nodiscard]] bool probe_encoder(const EncoderSettings &settings) const;

    std::unique_ptr<FrameSource> open_source(const std::string &path) const;

private:
    FFmpeg();
    ~FFmpeg();

    FFmpegBackend *backend{};
};

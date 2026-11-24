/*
 * Copyright (C) 2022 Kasm
 * Copyright (C) 2026 Kasm Technologies Corp.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NETWORK_IMAGE_CHANNEL_STREAM_H__
#define __NETWORK_IMAGE_CHANNEL_STREAM_H__

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <rdr/OutStream.h>

namespace network {

    inline constexpr size_t IMAGE_CHANNEL_HEADER_SIZE = sizeof(uint32_t) * 4;
    inline constexpr size_t IMAGE_CHANNEL_PIECE_SIZE = 16000;
    inline constexpr size_t IMAGE_CHANNEL_MIN_PIECE_SIZE = 1024;
    inline constexpr size_t IMAGE_STREAM_INITIAL_BUFSIZE = 1024 * 1024;
    inline constexpr size_t IMAGE_STREAM_MAX_BUFSIZE = 64 * 1024 * 1024;
    inline constexpr size_t IMAGE_CHANNEL_MAX_BUFFERED = 1024 * 1024;
    inline constexpr size_t IMAGE_CHANNEL_CONGESTED = IMAGE_CHANNEL_MAX_BUFFERED / 2;
    inline constexpr std::chrono::seconds IMAGE_STREAM_SHRINK_IDLE{10};

    // The browser reads the header with getUint32(..., true), so write little-endian whatever the host order is.
    inline void writeLE32(uint8_t *dst, uint32_t v) {
        dst[0] = static_cast<uint8_t>(v);
        dst[1] = static_cast<uint8_t>(v >> 8);
        dst[2] = static_cast<uint8_t>(v >> 16);
        dst[3] = static_cast<uint8_t>(v >> 24);
    }

    class ImageChannelStream : public rdr::OutStream {
    public:
        struct Sink {
            std::function<bool(const uint8_t *, size_t)> send;
            std::function<size_t()> buffered;
            std::function<size_t()> maxMessage;
        };

        ImageChannelStream();

        void setSink(Sink newSink) {
            sink = std::move(newSink);
        }
        void clearSink() {
            sink = Sink{};
        }
        bool hasSink() const {
            return static_cast<bool>(sink.send);
        }

        void setFrameNumber(uint32_t frame) {
            frameNumber = frame;
        }

        void flush() override;
        size_t length() override {
            return totalLength;
        }
        void overrun(size_t needed) override;

        // Held back until the channel drains so the full-frame resend isn't encoded into a congested channel.
        bool takeDropped() {
            if (!dropped || congested())
                return false;
            dropped = false;
            return true;
        }

        // Releases a buffer grown past its initial size once it has gone unused for IMAGE_STREAM_SHRINK_IDLE.
        void trimIfIdle();

        bool congested() const {
            return sink.buffered && sink.buffered() > IMAGE_CHANNEL_CONGESTED;
        }

    private:
        // The buffer reserves IMAGE_CHANNEL_HEADER_SIZE bytes ahead of the payload so each piece is sent in place.
        uint8_t *payload() const {
            return buffer ? buffer.get() + IMAGE_CHANNEL_HEADER_SIZE : nullptr;
        }

        std::unique_ptr<uint8_t[]> buffer;
        size_t capacity = 0;
        std::chrono::steady_clock::time_point lastLarge{};
        Sink sink;
        size_t totalLength = 0;
        uint32_t messageId = 0;
        uint32_t frameNumber = 0;
        bool dropped = false;
    };

} // namespace network

#endif

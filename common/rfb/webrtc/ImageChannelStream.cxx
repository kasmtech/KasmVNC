/*
 * Copyright (C) 2022 Kasm
 * Copyright (C) 2026 Kasm Technologies Corp.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <algorithm>
#include <cstring>

#include <rfb/webrtc/ImageChannelStream.h>
#include <rdr/Exception.h>
#include <rfb/LogWriter.h>

using namespace network;

static rfb::LogWriter vlog("ImageChannel");

ImageChannelStream::ImageChannelStream() :
    OutStream() {
    ptr = end = nullptr;
}

void ImageChannelStream::flush() {
    const size_t len = ptr - payload();
    ptr = payload();
    if (!len)
        return;

    totalLength += len;

    if (len > IMAGE_STREAM_INITIAL_BUFSIZE)
        lastLarge = std::chrono::steady_clock::now();

    if (!sink.send) {
        dropped = true;
        return;
    }

    size_t piece = IMAGE_CHANNEL_PIECE_SIZE;
    if (sink.maxMessage) {
        const size_t maxMsg = sink.maxMessage();
        if (maxMsg) {
            // The controller refuses such channels up front; never send a piece above the transport's limit.
            if (maxMsg <= IMAGE_CHANNEL_HEADER_SIZE) {
                vlog.error("DataChannel max message size %zu cannot fit a header, dropping message %u",
                    maxMsg, messageId);
                dropped = true;
                messageId++;
                return;
            }
            piece = std::min(piece, maxMsg - IMAGE_CHANNEL_HEADER_SIZE);
        }
    }

    const uint32_t pieces = static_cast<uint32_t>((len + piece - 1) / piece);

    // Each header overwrites the tail of the previous piece's payload, which has already been sent.
    uint8_t *src = payload();
    size_t left = len;
    for (uint32_t i = 0; i < pieces; i++) {
        if (sink.buffered && sink.buffered() > IMAGE_CHANNEL_MAX_BUFFERED) {
            dropped = true;
            break;
        }

        const size_t cur = std::min(left, piece);
        uint8_t *msg = src - IMAGE_CHANNEL_HEADER_SIZE;
        writeLE32(msg, messageId);
        writeLE32(msg + 4, i);
        writeLE32(msg + 8, pieces);
        writeLE32(msg + 12, frameNumber);

        if (!sink.send(msg, IMAGE_CHANNEL_HEADER_SIZE + cur)) {
            vlog.error("DataChannel send failed, dropping message %u", messageId);
            dropped = true;
            break;
        }
        src += cur;
        left -= cur;
    }

    messageId++;
}

void ImageChannelStream::overrun(size_t needed) {
    const size_t used = ptr - payload();

    if (needed > IMAGE_STREAM_MAX_BUFSIZE - used) {
        vlog.error("rect exceeds hard cap (%zu used, %zu needed, cap %zu)", used, needed, IMAGE_STREAM_MAX_BUFSIZE);
        ptr = payload();
        throw rdr::Exception("ImageChannelStream: rect exceeds buffer cap");
    }

    size_t cap = capacity ? capacity - IMAGE_CHANNEL_HEADER_SIZE : IMAGE_STREAM_INITIAL_BUFSIZE;
    while (cap - used < needed)
        cap = std::min(cap * 2, IMAGE_STREAM_MAX_BUFSIZE);

    if (cap + IMAGE_CHANNEL_HEADER_SIZE != capacity) {
        // new[] leaves the memory uninitialised; the stream only ever reads what it wrote.
        std::unique_ptr<uint8_t[]> grown(new uint8_t[IMAGE_CHANNEL_HEADER_SIZE + cap]);
        if (used)
            memcpy(grown.get() + IMAGE_CHANNEL_HEADER_SIZE, payload(), used);
        buffer = std::move(grown);
        capacity = IMAGE_CHANNEL_HEADER_SIZE + cap;
        if (cap > IMAGE_STREAM_INITIAL_BUFSIZE)
            lastLarge = std::chrono::steady_clock::now();
    }

    ptr = payload() + used;
    end = buffer.get() + capacity;
}

void ImageChannelStream::trimIfIdle() {
    if (capacity <= IMAGE_CHANNEL_HEADER_SIZE + IMAGE_STREAM_INITIAL_BUFSIZE || ptr != payload())
        return;
    if (std::chrono::steady_clock::now() - lastLarge < IMAGE_STREAM_SHRINK_IDLE)
        return;
    buffer.reset();
    capacity = 0;
    ptr = end = nullptr;
}

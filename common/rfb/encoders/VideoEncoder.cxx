/* Copyright (C) 2026 Kasm.  All Rights Reserved.
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

#include "rfb/encoders/VideoEncoder.h"

#include <rfb/webrtc/WebRTCMediaTransport.h>
#include <rfb/LogWriter.h>
#include <rfb/SConnection.h>

#include "KasmVideoConstants.h"
#include "utils.h"

static rfb::LogWriter vlog("VideoEncoderRoute");

namespace rfb {

    bool VideoEncoder::isRoutedOverWebRTC(uint8_t screenId) {
        auto *wt = conn->wrtcTransport(screenId);
        return conn->cp.supportsWebRTCMedia && wt && wt->isReady();
    }

    void VideoEncoder::routeEncodedRect(uint8_t screenId, const uint8_t *data, size_t size, bool isKeyframe,
        uint8_t codecId, uint8_t codecTypeId) {
        auto *wt = conn->wrtcTransport(screenId);
        const bool useWrtc = routedOverWebRTC && wt;
        auto *os = conn->getOutStream();
        // One read: the negotiated codec can change on the WebRTC thread between two calls.
        const auto negotiated = useWrtc ? wt->negotiatedCodec() : kasmVideoUnknownCodec;
        if (useWrtc && negotiated != kasmVideoUnknownCodec && negotiated != codecId) {
            conn->markWrtcCodecMismatch(screenId);
            writeVideoSkipRect(screenId);
            return;
        }
        if (useWrtc) {
            os->writeU8(screenId);
            os->writeU8(kasmVideoSkip);
            wt->pushEncoded(screenId, data, size, isKeyframe);
        } else {
            os->writeU8(screenId);
            os->writeU8(codecId);
            os->writeU8(codecTypeId);
            os->writeU8(isKeyframe ? 1 : 0);
            encoders::write_compact(os, static_cast<int>(size));
            os->writeBytes(data, size);
        }
    }

    void VideoEncoder::writeVideoSkipRect(uint8_t screenId) {
        auto *os = conn->getOutStream();
        os->writeU8(screenId);
        os->writeU8(kasmVideoSkip);
    }

} // namespace rfb

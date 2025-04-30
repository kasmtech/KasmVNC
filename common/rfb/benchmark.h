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

#include "CConnection.h"
#include "CMsgReader.h"
#include "LogWriter.h"

extern "C" {
#include <libavutil/frame.h>
}

static rfb::LogWriter vlog("Benchmarking");

namespace benchmarking {
    using namespace rfb;

    class MockTestConnection : public CConnection {
    public:
        virtual void setNewFrame(const AVFrame *frame) = 0;
    };

    static constexpr rdr::S32 default_encodings[] = {
        encodingTight,
        encodingZRLE,
        encodingHextile,
        encodingRRE,
        encodingRaw,
        pseudoEncodingCompressLevel9,
        pseudoEncodingQualityLevel9,
        pseudoEncodingFineQualityLevel100,
        pseudoEncodingSubsamp16X
        //pseudoEncodingWEBP
        //pseudoEncodingQOI
    };
}

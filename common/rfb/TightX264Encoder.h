/* Copyright (C) 2000-2003 Constantin Kaplinsky.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander
 * Copyright 2014 Pierre Ossman for Cendio AB
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
#ifndef __RFB_TIGHTX264ENCODER_H__
#define __RFB_TIGHTX264ENCODER_H__

#include <rfb/Encoder.h>
#include <stdint.h>
#include <vector>

struct x264_t;
struct x264_param_t;
struct Mp4Context;
struct Mp4State;

namespace rfb {

  class EncCache;

  class TightX264Encoder : public Encoder {
  public:
    TightX264Encoder(SConnection* conn, EncCache *encCache, uint8_t cacheType);
    virtual ~TightX264Encoder();

    virtual bool isSupported();

    virtual void setQualityLevel(int level) {}
    virtual void setFineQualityLevel(int quality, int subsampling) {}

    virtual void writeRect(const PixelBuffer* pb, const Palette& palette);
    virtual void writeSolidRect(int width, int height,
                                const PixelFormat& pf,
                                const rdr::U8* colour);

    virtual void setKeyframe() { keyframe = true; }

  protected:
    void writeCompact(rdr::U32 value, rdr::OutStream* os) const;
    void mp4_write_callback(const void *buffer, size_t size);

  protected:
    bool keyframe;
    x264_t *enc;
    x264_param_t *params;
    Mp4Context *mux;
    Mp4State *muxstate;
    unsigned framectr;

    EncCache *encCache;
    uint8_t cacheType;
  public:
    uint8_t *framebuf;
    uint32_t framelen;
  };
}
#endif

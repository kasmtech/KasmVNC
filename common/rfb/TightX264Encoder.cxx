/* Copyright (C) 2000-2003 Constantin Kaplinsky.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
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
#include <rdr/OutStream.h>
#include <rfb/EncCache.h>
#include <rfb/encodings.h>
#include <rfb/LogWriter.h>
#include <rfb/SConnection.h>
#include <rfb/ServerCore.h>
#include <rfb/PixelBuffer.h>
#include <rfb/TightX264Encoder.h>
#include <rfb/TightConstants.h>

#include <webp/encode.h>
#include "nvidia.h"
#include "mp4.h"

#define MAX_FRAMELEN (1024 * 1024)

using namespace rfb;

static LogWriter vlog("x264");

static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);
static const PixelFormat pfBGRX(32, 24, false, true, 255, 255, 255, 16, 8, 0);

TightX264Encoder::TightX264Encoder(SConnection* conn, EncCache *cache_, uint8_t cacheType_) :
  Encoder(conn, encodingTight, (EncoderFlags)(EncoderUseNativePF | EncoderLossy), -1),
  keyframe(true), mux(NULL), muxstate(NULL), framectr(0),
  nvidia_init_done(false),
  encCache(cache_), cacheType(cacheType_),
  framebuf(NULL), framelen(0), bitbuf(NULL)
{
  framebuf = new uint8_t[MAX_FRAMELEN];
  bitbuf = new uint8_t[MAX_FRAMELEN];
  mux = new Mp4Context;
  memset(mux, 0, sizeof(Mp4Context));
  muxstate = new Mp4State;
  memset(muxstate, 0, sizeof(Mp4State));
}

TightX264Encoder::~TightX264Encoder()
{
  delete [] framebuf;
  delete [] bitbuf;
  delete mux;
  delete muxstate;
}

bool TightX264Encoder::isSupported()
{
  if (!conn->cp.supportsEncoding(encodingTight))
    return false;

  // Unconditional support if enabled
  return rfb::Server::x264Bitrate != 0;
}

void TightX264Encoder::mp4_write_callback(const void *buffer, size_t size)
{
  if (framelen + size > MAX_FRAMELEN)
    vlog.error("Tried to write too large a frame, %lu bytes", framelen + size);

  memcpy(&framebuf[framelen], buffer, size);
  framelen += size;
}

void TightX264Encoder::writeRect(const PixelBuffer* pb, const Palette& palette)
{
  const rdr::U8* buffer;
  int stride;

  rdr::OutStream* os;

  if (pb->width() < 320)
    return; // Sometimes we get sent an 1x1 frame, or a cursor

  uint32_t w, h;
  w = pb->width();
  h = pb->height();

  /*w += w & 1;
  h += h & 1;*/

  if (!nvidia_init_done) {
    if (nvidia_init(w, h, rfb::Server::x264Bitrate,
                rfb::Server::frameRate) != 0) {
      vlog.error("nvidia init failed");
    }
    nvidia_init_done = true;
  }

  os = conn->getOutStream();

  uint32_t cachelen;
  const void *cachedata;
  if (encCache->enabled &&
      (cachedata = encCache->get(cacheType, framectr, 0, w, h, cachelen))) {
    os->writeU8(tightX264 << 4);
    writeCompact(cachelen, os);
    os->writeBytes(cachedata, cachelen);
    framectr++;
    return;
  }

  if (keyframe) {
    framectr = 0;
    keyframe = false;

    free(mux->buf_header.buf);
    free(mux->buf_mdat.buf);
    free(mux->buf_moof.buf);
    memset(mux, 0, sizeof(Mp4Context));
    memset(muxstate, 0, sizeof(Mp4State));
  }

  mux->framerate = rfb::Server::frameRate;
  mux->w = w;
  mux->h = h;

  buffer = pb->getBuffer(pb->getRect(), &stride);

  // Convert it to yuv420 using libwebp's helper functions
/*  WebPPicture pic;

  WebPPictureInit(&pic);
  pic.width = pb->getRect().width();
  pic.height = pb->getRect().height();
*/
  bool freebuffer = false;
  /*if (pic.width & 1 || pic.height & 1) {
    // Expand to divisible-by-2 for x264
    freebuffer = true;
    const uint32_t oldw = pic.width;
    const uint32_t oldh = pic.height;
    pic.width += pic.width & 1;
    pic.height += pic.height & 1;
    stride = pic.width;
    const rdr::U8 *oldbuffer = buffer;
    buffer = (const rdr::U8*) calloc(pic.width * pic.height, 4);

    uint32_t y;
    for (y = 0; y < oldh; y++)
      memcpy((void *) &buffer[y * stride * 4], &oldbuffer[y * oldw * 4], oldw * 4);
  }*/

/*  if (pfRGBX.equal(pb->getPF())) {
    WebPPictureImportRGBX(&pic, buffer, stride * 4);
  } else if (pfBGRX.equal(pb->getPF())) {
    WebPPictureImportBGRX(&pic, buffer, stride * 4);
  } else {
    rdr::U8* tmpbuf = new rdr::U8[pic.width * pic.height * 3];
    pb->getPF().rgbFromBuffer(tmpbuf, (const rdr::U8 *) buffer, pic.width, stride, pic.height);
    stride = pic.width * 3;

    WebPPictureImportRGB(&pic, tmpbuf, stride);
    delete [] tmpbuf;
  }*/

  if (!pfBGRX.equal(pb->getPF())) {
    vlog.error("unsupported pixel format");
    return;
  }

  if (freebuffer)
    free((void *) buffer);

  // Encode
  uint32_t bitlen;
  if (nvenc_frame(buffer, framectr++, bitbuf, bitlen) != 0) {
    vlog.error("encoding failed");
    return;
  }

  // Need to parse NALs out of the stream
  const uint8_t prefix[3] = { 0, 0, 1 };
  const uint8_t *nalptr = bitbuf;
  int i_nals = 0;
  const uint8_t *nalstarts[32] = { NULL };
  uint32_t nallens[32] = { 0 };
  uint32_t remlen = bitlen;

  while (1) {
    const uint8_t *next = (uint8_t *) memmem(nalptr, remlen, prefix, 3);
    if (!next)
      break;

    remlen -= (next + 3) - nalptr;
    nalptr = nalstarts[i_nals] = next + 3;

    i_nals++;
  };

  // Lens
  int i;
  for (i = 0; i < i_nals; i++) {
    if (i == i_nals - 1) {
      nallens[i] = bitbuf + bitlen - nalstarts[i];
    } else {
      nallens[i] = nalstarts[i + 1] - nalstarts[i] - 3;
    }
  }

  // Mux
  framelen = 0;
  os->writeU8(tightX264 << 4);

  for (i = 0; i < i_nals; i++) {
    uint32_t pack_len = nallens[i];
    const uint8_t *pack_data = nalstarts[i];

    struct NAL nal; nal_parse_header(&nal, pack_data[0]);

    switch (nal.unit_type) {
      case NalUnitType_SPS: { set_sps(mux, pack_data, pack_len); break; }
      case NalUnitType_PPS: { set_pps(mux, pack_data, pack_len); break; }
      case NalUnitType_CodedSliceIdr:
      case NalUnitType_CodedSliceNonIdr: {
          // Write all remaining NALs under the assumption they are the same type.
          const uint32_t origlen = pack_len;
          pack_len = bitbuf + bitlen - pack_data;
          set_slice(mux, pack_data, origlen, pack_len, nal.unit_type);
          break;
      }
      default: break;
    }

    if (nal.unit_type != NalUnitType_CodedSliceIdr &&
        nal.unit_type != NalUnitType_CodedSliceNonIdr)
        continue;

    enum BufError err;
    if (!muxstate->header_sent) {
      struct BitBuf header_buf;
      err = get_header(mux, &header_buf); chk_err_continue
      mp4_write_callback(header_buf.buf, header_buf.offset);

      muxstate->sequence_number = 1;
      muxstate->base_data_offset = header_buf.offset;
      muxstate->base_media_decode_time = 0;
      muxstate->header_sent = true;
      muxstate->nals_count = 0;
      muxstate->default_sample_duration = default_sample_size;
    }

    err = set_mp4_state(mux, muxstate); chk_err_continue
    {
      struct BitBuf moof_buf;
      err = get_moof(mux, &moof_buf); chk_err_continue
      mp4_write_callback(moof_buf.buf, moof_buf.offset);
    }
    {
      struct BitBuf mdat_buf;
      err = get_mdat(mux, &mdat_buf); chk_err_continue
      mp4_write_callback(mdat_buf.buf, mdat_buf.offset);
    }

    break;
  }

  if (encCache->enabled) {
    void *tmp = malloc(framelen);
    memcpy(tmp, framebuf, framelen);
    encCache->add(cacheType, framectr, 0, w, h, framelen, tmp);
  }

  writeCompact(framelen, os);
  os->writeBytes(framebuf, framelen);

  // Cleanup
  //WebPPictureFree(&pic);
}

void TightX264Encoder::writeSolidRect(int width, int height,
                                      const PixelFormat& pf,
                                      const rdr::U8* colour)
{
  // FIXME: Add a shortcut in the X264 compressor to handle this case
  //        without having to use the default fallback which is very slow.
  Encoder::writeSolidRect(width, height, pf, colour);
}

void TightX264Encoder::writeCompact(rdr::U32 value, rdr::OutStream* os) const
{
  // Copied from TightEncoder as it's overkill to inherit just for this
  rdr::U8 b;

  b = value & 0x7F;
  if (value <= 0x7F) {
    os->writeU8(b);
  } else {
    os->writeU8(b | 0x80);
    b = value >> 7 & 0x7F;
    if (value <= 0x3FFF) {
      os->writeU8(b);
    } else {
      os->writeU8(b | 0x80);
      os->writeU8(value >> 14 & 0xFF);
    }
  }
}

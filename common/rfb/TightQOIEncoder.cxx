/* Copyright (C) 2019 Kasm Web
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
#include <rfb/encodings.h>
#include <rfb/LogWriter.h>
#include <rfb/SConnection.h>
#include <rfb/ServerCore.h>
#include <rfb/PixelBuffer.h>
#include <rfb/TightQOIEncoder.h>
#include <rfb/TightConstants.h>
#include <rfb/util.h>
#include <sys/time.h>
#include <stdlib.h>

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi.h"

using namespace rfb;
static LogWriter vlog("QOI");

static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);
static const PixelFormat pfBGRX(32, 24, false, true, 255, 255, 255, 16, 8, 0);

// An optimized version that assumes 4-alignment and RGBX/BGRX
static void *qoi_encode_kasm(const void *data, const qoi_desc *desc, int *out_len,
                             const unsigned isrgb, const unsigned stride) {
	int i, max_size, p, run;
	unsigned px_len, px_end, px_pos, y, x;
	unsigned char *bytes;
	const uint32_t *pixels;
	qoi_rgba_t index[64];
	qoi_rgba_t px, px_prev;

	if (
		data == NULL || out_len == NULL || desc == NULL ||
		desc->width == 0 || desc->height == 0 ||
		desc->channels < 3 || desc->channels > 4 ||
		desc->colorspace > 1 ||
		desc->height >= QOI_PIXELS_MAX / desc->width
	) {
		return NULL;
	}

	max_size =
		desc->width * desc->height * (3 + 1) +
		QOI_HEADER_SIZE + sizeof(qoi_padding);

	p = 0;
	bytes = (unsigned char *) QOI_MALLOC(max_size);
	if (!bytes) {
		return NULL;
	}

	qoi_write_32(bytes, &p, QOI_MAGIC);
	qoi_write_32(bytes, &p, desc->width);
	qoi_write_32(bytes, &p, desc->height);
	bytes[p++] = 3;
	bytes[p++] = desc->colorspace;


	pixels = (const uint32_t *)data;

	QOI_ZEROARR(index);

	run = 0;
	px_prev.rgba.r = 0;
	px_prev.rgba.g = 0;
	px_prev.rgba.b = 0;
	px_prev.rgba.a = 255;
	px = px_prev;

	px_len = desc->width * desc->height;
	px_end = px_len - 1;

	px_pos = 0;
	for (y = 0; y < desc->height; y++) {
		for (x = 0; x < desc->width; x++, px_pos++) {
			const unsigned stridedpos = y * stride + x;

			px.v = pixels[stridedpos];
			if (!isrgb) {
				uint8_t tmp = px.rgba.r;
				px.rgba.r = px.rgba.b;
				px.rgba.b = tmp;
			}

			if (px.v == px_prev.v) {
				run++;
				if (run == 62 || px_pos == px_end) {
					bytes[p++] = QOI_OP_RUN | (run - 1);
					run = 0;
				}
			} else {
				if (run > 0) {
					bytes[p++] = QOI_OP_RUN | (run - 1);
					run = 0;
				}

				signed char vr = px.rgba.r - px_prev.rgba.r;
				signed char vg = px.rgba.g - px_prev.rgba.g;
				signed char vb = px.rgba.b - px_prev.rgba.b;

				signed char vg_r = vr - vg;
				signed char vg_b = vb - vg;

				if (
					vr > -3 && vr < 2 &&
					vg > -3 && vg < 2 &&
					vb > -3 && vb < 2
				) {
					bytes[p++] = QOI_OP_DIFF | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2);
				} else if (
					vg_r >  -9 && vg_r <  8 &&
					vg   > -33 && vg   < 32 &&
					vg_b >  -9 && vg_b <  8
				) {
					bytes[p++] = QOI_OP_LUMA     | (vg   + 32);
					bytes[p++] = (vg_r + 8) << 4 | (vg_b +  8);
				} else {
					bytes[p++] = QOI_OP_RGB;
					bytes[p++] = px.rgba.r;
					bytes[p++] = px.rgba.g;
					bytes[p++] = px.rgba.b;
				}
			}
			px_prev = px;
		}
	}

	for (i = 0; i < (int)sizeof(qoi_padding); i++) {
		bytes[p++] = qoi_padding[i];
	}

	*out_len = p;
	return bytes;
}

TightQOIEncoder::TightQOIEncoder(SConnection* conn) :
  Encoder(conn, encodingTight, (EncoderFlags)(EncoderUseNativePF), -1)
{
}

TightQOIEncoder::~TightQOIEncoder()
{
}

bool TightQOIEncoder::isSupported()
{
  if (!conn->cp.supportsEncoding(encodingTight))
    return false;

  if (conn->cp.supportsQOI)
    return true;

  // Tight support, but not QOI
  return false;
}

void TightQOIEncoder::compressOnly(const PixelBuffer* pb, const uint8_t qualityIn,
                                    std::vector<uint8_t> &out, const bool lowVideoQuality) const
{
  const rdr::U8* buffer;
  int stride, len;
  qoi_desc desc;
  void *encoded;

  buffer = pb->getBuffer(pb->getRect(), &stride);

  desc.width = pb->getRect().width();
  desc.height = pb->getRect().height();
  desc.colorspace = QOI_LINEAR;
  desc.channels = 4;

  encoded = qoi_encode_kasm(buffer, &desc, &len, pfRGBX.equal(pb->getPF()), stride);

  if (!encoded) {
    // Error
    vlog.error("QOI error");
  }

  out.resize(len);
  memcpy(&out[0], encoded, len);

  free(encoded);
}

void TightQOIEncoder::writeOnly(const std::vector<uint8_t> &out) const
{
  rdr::OutStream* os;

  os = conn->getOutStream();

  os->writeU8(tightQoi << 4);

  writeCompact(out.size(), os);
  os->writeBytes(&out[0], out.size());
}

void TightQOIEncoder::writeRect(const PixelBuffer* pb, const Palette& palette)
{
  rdr::OutStream* os;
  const rdr::U8* buffer;
  int stride, len;
  qoi_desc desc;
  void *encoded;

  buffer = pb->getBuffer(pb->getRect(), &stride);

  desc.width = pb->getRect().width();
  desc.height = pb->getRect().height();
  desc.colorspace = QOI_LINEAR;
  desc.channels = 4;

  encoded = qoi_encode_kasm(buffer, &desc, &len, pfRGBX.equal(pb->getPF()), stride);

  if (!encoded) {
    // Error
    vlog.error("QOI error");
  }

  os = conn->getOutStream();

  os->writeU8(tightQoi << 4);

  writeCompact(len, os);
  os->writeBytes(encoded, len);

  free(encoded);
}

void TightQOIEncoder::writeSolidRect(int width, int height,
                                      const PixelFormat& pf,
                                      const rdr::U8* colour)
{
  // FIXME: Add a shortcut in the JPEG compressor to handle this case
  //        without having to use the default fallback which is very slow.
  Encoder::writeSolidRect(width, height, pf, colour);
}

void TightQOIEncoder::writeCompact(rdr::U32 value, rdr::OutStream* os) const
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

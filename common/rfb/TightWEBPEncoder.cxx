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
#include <rfb/TightWEBPEncoder.h>
#include <rfb/TightConstants.h>
#include <rfb/util.h>
#include <sys/time.h>
#include <stdlib.h>

#include <webp/encode.h>

using namespace rfb;
static LogWriter vlog("WEBP");

static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);
static const PixelFormat pfBGRX(32, 24, false, true, 255, 255, 255, 16, 8, 0);

struct TightWEBPConfiguration {
    uint8_t quality;
    uint8_t method;
};

// Matt: I found that -q 40 -m 1 can compress an HD image in 78ms and the quality is
// acceptable and similar to cjpeg -q 70

// Using a test image from a browser, I matched DSSIM levels with the below JPEG
// targets. Qualities 6 and above couldn't match jpeg, presumably because
// webp always subsamples. Each level is 50-70% of the corresponding JPEG
// level's size, while approx matching its quality.

// NOTE:  The JPEG quality and subsampling levels below were obtained
// experimentally by the VirtualGL Project.  They represent the approximate
// average compression ratios listed below, as measured across the set of
// every 10th frame in the SPECviewperf 9 benchmark suite.
//
// 9 = JPEG quality 100, no subsampling (ratio ~= 10:1)
//     [this should be lossless, except for round-off error]
// 8 = JPEG quality 92,  no subsampling (ratio ~= 20:1)
//     [this should be perceptually lossless, based on current research]
// 7 = JPEG quality 86,  no subsampling (ratio ~= 25:1)
// 6 = JPEG quality 79,  no subsampling (ratio ~= 30:1)
// 5 = JPEG quality 77,  4:2:2 subsampling (ratio ~= 40:1)
// 4 = JPEG quality 62,  4:2:2 subsampling (ratio ~= 50:1)
// 3 = JPEG quality 42,  4:2:2 subsampling (ratio ~= 60:1)
// 2 = JPEG quality 41,  4:2:0 subsampling (ratio ~= 70:1)
// 1 = JPEG quality 29,  4:2:0 subsampling (ratio ~= 80:1)
// 0 = JPEG quality 15,  4:2:0 subsampling (ratio ~= 100:1)

static const struct TightWEBPConfiguration conf[10] = {
  {  5, 0 }, // 0
  {  24, 0 }, // 1
  {  30, 0 }, // 2
  {  37, 0 }, // 3
  {  42, 0 }, // 4
  {  65, 0 }, // 5
  {  78, 0 }, // 6
  {  85, 0 }, // 7
  {  88, 0 }, // 8
  { 100, 0 }  // 9
};


TightWEBPEncoder::TightWEBPEncoder(SConnection* conn) :
  Encoder(conn, encodingTight, (EncoderFlags)(EncoderUseNativePF | EncoderLossy), -1),
  qualityLevel(-1)
{
}

TightWEBPEncoder::~TightWEBPEncoder()
{
}

bool TightWEBPEncoder::isSupported()
{
  if (!conn->cp.supportsEncoding(encodingTight))
    return false;

  if (conn->cp.supportsWEBP)
    return true;

  // Tight support, but not WEBP
  return false;
}

void TightWEBPEncoder::setQualityLevel(int level)
{
  qualityLevel = level;
}

void TightWEBPEncoder::setFineQualityLevel(int quality, int subsampling)
{
  // NOP
}

bool TightWEBPEncoder::treatLossless()
{
  return qualityLevel >= rfb::Server::treatLossless;
}

void TightWEBPEncoder::compressOnly(const PixelBuffer* pb, const uint8_t qualityIn,
                                    std::vector<uint8_t> &out, const bool lowVideoQuality) const
{
  const rdr::U8* buffer;
  int stride;
  uint8_t quality, method;
  WebPConfig cfg;
  WebPPicture pic;
  WebPMemoryWriter wrt;

  buffer = pb->getBuffer(pb->getRect(), &stride);

  if (lowVideoQuality) {
    if (rfb::Server::webpVideoQuality == -1) {
      quality = 3;
      method = 0;
    } else {
      uint8_t num = rfb::Server::webpVideoQuality;
      quality = conf[num].quality;
      method = conf[num].method;
    }
  } else if (qualityIn <= 9) {
    quality = conf[qualityIn].quality;
    method = conf[qualityIn].method;
  } else {
    quality = 8;
    method = 0;
  }

  WebPConfigInit(&cfg);
  cfg.method = method;
  cfg.quality = quality;
  cfg.thread_level = 1; // Try to use multiple threads

  WebPPictureInit(&pic);
  pic.width = pb->getRect().width();
  pic.height = pb->getRect().height();

  if (pfRGBX.equal(pb->getPF())) {
    WebPPictureImportRGBX(&pic, buffer, stride * 4);
  } else if (pfBGRX.equal(pb->getPF())) {
    WebPPictureImportBGRX(&pic, buffer, stride * 4);
  } else {
    rdr::U8* tmpbuf = new rdr::U8[pic.width * pic.height * 3];
    pb->getPF().rgbFromBuffer(tmpbuf, (const rdr::U8 *) buffer, pic.width, stride, pic.height);
    stride = pic.width * 3;

    WebPPictureImportRGB(&pic, tmpbuf, stride);
    delete [] tmpbuf;
  }

  WebPMemoryWriterInit(&wrt);
  pic.writer = WebPMemoryWrite;
  pic.custom_ptr = &wrt;

  if (!WebPEncode(&cfg, &pic)) {
    // Error
    vlog.error("WEBP error %u", pic.error_code);
  }

  out.resize(wrt.size);
  memcpy(&out[0], wrt.mem, wrt.size);

  WebPPictureFree(&pic);
  WebPMemoryWriterClear(&wrt);
}

void TightWEBPEncoder::writeOnly(const std::vector<uint8_t> &out) const
{
  rdr::OutStream* os;

  os = conn->getOutStream(conn->cp.supportsUdp);

  os->writeU8(tightWebp << 4);

  writeCompact(out.size(), os);
  os->writeBytes(&out[0], out.size());
}

void TightWEBPEncoder::writeRect(const PixelBuffer* pb, const Palette& palette)
{
  const rdr::U8* buffer;
  int stride;
  uint8_t quality, method;
  WebPConfig cfg;
  WebPPicture pic;
  WebPMemoryWriter wrt;

  rdr::OutStream* os;

  buffer = pb->getBuffer(pb->getRect(), &stride);

  if (qualityLevel >= 0 && qualityLevel <= 9) {
    quality = conf[qualityLevel].quality;
    method = conf[qualityLevel].method;
  } else {
    quality = 8;
    method = 0;
  }

  WebPConfigInit(&cfg);
  cfg.method = method;
  cfg.quality = quality;
  cfg.thread_level = 1; // Try to use multiple threads

  WebPPictureInit(&pic);
  pic.width = pb->getRect().width();
  pic.height = pb->getRect().height();

  if (pfRGBX.equal(pb->getPF())) {
    WebPPictureImportRGBX(&pic, buffer, stride * 4);
  } else if (pfBGRX.equal(pb->getPF())) {
    WebPPictureImportBGRX(&pic, buffer, stride * 4);
  } else {
    rdr::U8* tmpbuf = new rdr::U8[pic.width * pic.height * 3];
    pb->getPF().rgbFromBuffer(tmpbuf, (const rdr::U8 *) buffer, pic.width, stride, pic.height);
    stride = pic.width * 3;

    WebPPictureImportRGB(&pic, tmpbuf, stride);
    delete [] tmpbuf;
  }

  WebPMemoryWriterInit(&wrt);
  pic.writer = WebPMemoryWrite;
  pic.custom_ptr = &wrt;

  if (!WebPEncode(&cfg, &pic)) {
    // Error
    vlog.error("WEBP error %u", pic.error_code);
  }

  os = conn->getOutStream(conn->cp.supportsUdp);

  os->writeU8(tightWebp << 4);

  writeCompact(wrt.size, os);
  os->writeBytes(wrt.mem, wrt.size);

  WebPPictureFree(&pic);
  WebPMemoryWriterClear(&wrt);
}

// How many milliseconds would it take to encode a 256x256 block at quality 5
rdr::U32 TightWEBPEncoder::benchmark() const
{
  rdr::U8* buffer;
  struct timeval start;
  int stride, i;
  // the minimum WebP quality settings used in KasmVNC
  const uint8_t quality = 5, method = 0; 
  WebPConfig cfg;
  WebPPicture pic;
  WebPMemoryWriter wrt;
  ManagedPixelBuffer pb(pfRGBX, 256, 256);

  buffer = pb.getBufferRW(pb.getRect(), &stride);
  // Fill it with random data
  for (i = 0; i < pb.getRect().width() * pb.getRect().height() * 4; i++)
    buffer[i] = random();

  gettimeofday(&start, NULL);

  WebPConfigInit(&cfg);
  cfg.method = method;
  cfg.quality = quality;
  cfg.thread_level = 1; // Try to use multiple threads

  WebPPictureInit(&pic);
  pic.width = pb.getRect().width();
  pic.height = pb.getRect().height();

  WebPPictureImportRGBX(&pic, buffer, stride * 4);

  WebPMemoryWriterInit(&wrt);
  pic.writer = WebPMemoryWrite;
  pic.custom_ptr = &wrt;

  if (!WebPEncode(&cfg, &pic)) {
    // Error
    vlog.error("WEBP error %u", pic.error_code);
  }

  WebPPictureFree(&pic);
  WebPMemoryWriterClear(&wrt);

  return msSince(&start);
}

void TightWEBPEncoder::writeSolidRect(int width, int height,
                                      const PixelFormat& pf,
                                      const rdr::U8* colour)
{
  // FIXME: Add a shortcut in the JPEG compressor to handle this case
  //        without having to use the default fallback which is very slow.
  Encoder::writeSolidRect(width, height, pf, colour);
}

void TightWEBPEncoder::writeCompact(rdr::U32 value, rdr::OutStream* os) const
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

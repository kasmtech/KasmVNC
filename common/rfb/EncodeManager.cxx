/* Copyright (C) 2000-2003 Constantin Kaplinsky.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
 * Copyright 2014-2018 Pierre Ossman for Cendio AB
 * Copyright (C) 2018 Lauri Kasanen
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

#include <cstdlib>
#include <rfb/cpuid.h>
#include <rfb/EncCache.h>
#include <rfb/EncodeManager.h>
#include <rfb/Encoder.h>
#include <rfb/Palette.h>
#include <rfb/scale_sse2.h>
#include <rfb/SConnection.h>
#include <rfb/ServerCore.h>
#include <rfb/SMsgWriter.h>
#include <rfb/UpdateTracker.h>
#include <rfb/LogWriter.h>
#include <rfb/Exception.h>
#include <rfb/Watermark.h>

#include <rfb/RawEncoder.h>
#include <rfb/RREEncoder.h>
#include <rfb/HextileEncoder.h>
#include <rfb/ZRLEEncoder.h>
#include <rfb/TightEncoder.h>
#include <rfb/TightJPEGEncoder.h>
#include <rfb/TightWEBPEncoder.h>
#include <rfb/TightQOIEncoder.h>
#include <execution>
#include <tbb/parallel_for.h>

using namespace rfb;

static LogWriter vlog("EncodeManager");

// If this rect was touched this update, add this to its quality score
#define SCORE_INCREMENT 32

// Split each rectangle into smaller ones no larger than this area,
// and no wider than this width.
static const int SubRectMaxArea = 65536;
static const int SubRectMaxWidth = 2048;

// The size in pixels of either side of each block tested when looking
// for solid blocks.
static const int SolidSearchBlock = 16;
// Don't bother with blocks smaller than this
static const int SolidBlockMinArea = 2048;

namespace rfb {

enum EncoderClass {
  encoderRaw,
  encoderRRE,
  encoderHextile,
  encoderTight,
  encoderTightJPEG,
  encoderTightWEBP,
  encoderTightQOI,
  encoderZRLE,
  encoderClassMax,
};

enum EncoderType {
  encoderSolid,
  encoderBitmap,
  encoderBitmapRLE,
  encoderIndexed,
  encoderIndexedRLE,
  encoderFullColour,
  encoderTypeMax,
};

struct RectInfo {
  int rleRuns;
  Palette *palette;
};

struct QualityInfo {
  struct timeval lastUpdate{};
  Rect rect;
  unsigned score{};
};

};

static const char *encoderClassName(EncoderClass klass)
{
  switch (klass) {
  case encoderRaw:
    return "Raw";
  case encoderRRE:
    return "RRE";
  case encoderHextile:
    return "Hextile";
  case encoderTight:
    return "Tight";
  case encoderTightJPEG:
    return "Tight (JPEG)";
  case encoderTightWEBP:
    return "Tight (WEBP)";
  case encoderTightQOI:
    return "Tight (QOI)";
  case encoderZRLE:
    return "ZRLE";
  case encoderClassMax:
    break;
  }

  return "Unknown Encoder Class";
}

static const char *encoderTypeName(EncoderType type)
{
  switch (type) {
  case encoderSolid:
    return "Solid";
  case encoderBitmap:
    return "Bitmap";
  case encoderBitmapRLE:
    return "Bitmap RLE";
  case encoderIndexed:
    return "Indexed";
  case encoderIndexedRLE:
    return "Indexed RLE";
  case encoderFullColour:
    return "Full Colour";
  case encoderTypeMax:
    break;
  }

  return "Unknown Encoder Type";
}

static void updateMaxVideoRes(uint16_t *x, uint16_t *y) {
  sscanf(Server::maxVideoResolution, "%hux%hu", x, y);
  *x &= ~1;
  *y &= ~1;

  if (*x < 16 || *x > 2048 ||
      *y < 16 || *y > 2048) {
    *x = 1920;
    *y = 1080;
  }
}

EncodeManager::EncodeManager(SConnection* conn_, EncCache *encCache_) : conn(conn_),
  dynamicQualityMin(-1), dynamicQualityOff(-1),
  areaCur(0), videoDetected(false), videoTimer(this),
  watermarkStats(0),
  maxEncodingTime(0), framesSinceEncPrint(0),
  encCache(encCache_)
{
  StatsVector::iterator iter;

  encoders.resize(encoderClassMax, NULL);
  activeEncoders.resize(encoderTypeMax, encoderRaw);

  encoders[encoderRaw] = new RawEncoder(conn);
  encoders[encoderRRE] = new RREEncoder(conn);
  encoders[encoderHextile] = new HextileEncoder(conn);
  encoders[encoderTight] = new TightEncoder(conn);
  encoders[encoderTightJPEG] = new TightJPEGEncoder(conn);
  encoders[encoderTightWEBP] = new TightWEBPEncoder(conn);
  encoders[encoderTightQOI] = new TightQOIEncoder(conn);
  encoders[encoderZRLE] = new ZRLEEncoder(conn);

  webpBenchResult = ((TightWEBPEncoder *) encoders[encoderTightWEBP])->benchmark();
  vlog.info("WEBP benchmark result: %u ms", webpBenchResult);

  unsigned videoTime = rfb::Server::videoTime;
  if (videoTime < 1) videoTime = 1;
  //areaPercentages = new unsigned char[videoTime * rfb::Server::frameRate]();
  // maximum possible values, as they may change later at runtime
  areaPercentages = new unsigned char[2000 * 60]();

  if (!rfb::Server::videoTime)
    videoDetected = true;

  updateMaxVideoRes(&maxVideoX, &maxVideoY);

  updates = 0;
  memset(&copyStats, 0, sizeof(copyStats));
  stats.resize(encoderClassMax);
  for (iter = stats.begin();iter != stats.end();++iter) {
    StatsVector::value_type::iterator iter2;
    iter->resize(encoderTypeMax);
    for (iter2 = iter->begin();iter2 != iter->end();++iter2)
      memset(&*iter2, 0, sizeof(EncoderStats));
  }

  if (Server::dynamicQualityMax && Server::dynamicQualityMax <= 9 &&
      Server::dynamicQualityMax > Server::dynamicQualityMin) {
    dynamicQualityMin = Server::dynamicQualityMin;
    dynamicQualityOff = Server::dynamicQualityMax - Server::dynamicQualityMin;
  }

    const auto num_cores = cpu_info::cores_count;
    arena.initialize(num_cores);
}

EncodeManager::~EncodeManager()
{
  std::vector<Encoder*>::iterator iter;

  logStats();

  delete [] areaPercentages;

  for (iter = encoders.begin();iter != encoders.end();iter++)
    delete *iter;

  for (std::list<QualityInfo*>::iterator it = qualityList.begin(); it != qualityList.end(); it++)
    delete *it;
}

void EncodeManager::logStats()
{
  size_t i, j;

  unsigned rects;
  unsigned long long pixels, bytes, equivalent;

  double ratio;

  char a[1024], b[1024];

  rects = 0;
  pixels = bytes = equivalent = 0;

  vlog.info("Framebuffer updates: %u", updates);

  if (copyStats.rects != 0) {
    vlog.info("  %s:", "CopyRect");

    rects += copyStats.rects;
    pixels += copyStats.pixels;
    bytes += copyStats.bytes;
    equivalent += copyStats.equivalent;

    ratio = (double)copyStats.equivalent / copyStats.bytes;

    siPrefix(copyStats.rects, "rects", a, sizeof(a));
    siPrefix(copyStats.pixels, "pixels", b, sizeof(b));
    vlog.info("    %s: %s, %s", "Copies", a, b);
    iecPrefix(copyStats.bytes, "B", a, sizeof(a));
    vlog.info("    %*s  %s (1:%g ratio)",
              (int)strlen("Copies"), "",
              a, ratio);
  }

  for (i = 0;i < stats.size();i++) {
    // Did this class do anything at all?
    for (j = 0;j < stats[i].size();j++) {
      if (stats[i][j].rects != 0)
        break;
    }
    if (j == stats[i].size())
      continue;

    vlog.info("  %s:", encoderClassName((EncoderClass)i));

    for (j = 0;j < stats[i].size();j++) {
      if (stats[i][j].rects == 0)
        continue;

      rects += stats[i][j].rects;
      pixels += stats[i][j].pixels;
      bytes += stats[i][j].bytes;
      equivalent += stats[i][j].equivalent;

      ratio = (double)stats[i][j].equivalent / stats[i][j].bytes;

      siPrefix(stats[i][j].rects, "rects", a, sizeof(a));
      siPrefix(stats[i][j].pixels, "pixels", b, sizeof(b));
      vlog.info("    %s: %s, %s", encoderTypeName((EncoderType)j), a, b);
      iecPrefix(stats[i][j].bytes, "B", a, sizeof(a));
      vlog.info("    %*s  %s (1:%g ratio)",
                (int)strlen(encoderTypeName((EncoderType)j)), "",
                a, ratio);
    }
  }

  ratio = (double)equivalent / bytes;

  siPrefix(rects, "rects", a, sizeof(a));
  siPrefix(pixels, "pixels", b, sizeof(b));
  vlog.info("  Total: %s, %s", a, b);
  iecPrefix(bytes, "B", a, sizeof(a));
  vlog.info("         %s (1:%g ratio)", a, ratio);

  if (watermarkData) {
    siPrefix(watermarkStats, "B", a, sizeof(a));
    vlog.info("  Watermark data sent: %s", a);
  }
}

bool EncodeManager::supported(int encoding)
{
  switch (encoding) {
  case encodingRaw:
  case encodingRRE:
  case encodingHextile:
  case encodingZRLE:
  case encodingTight:
    return true;
  default:
    return false;
  }
}

bool EncodeManager::needsLosslessRefresh(const Region& req)
{
  return !lossyRegion.intersect(req).is_empty();
}

void EncodeManager::pruneLosslessRefresh(const Region& limits)
{
  lossyRegion.assign_intersect(limits);
}

void EncodeManager::writeUpdate(const UpdateInfo& ui, const PixelBuffer* pb,
                                const RenderedCursor* renderedCursor,
                                size_t maxUpdateSize)
{
    curMaxUpdateSize = maxUpdateSize;
    doUpdate(true, ui.changed, ui.copied, ui.copy_delta, ui.copypassed, pb, renderedCursor);
}

void EncodeManager::writeLosslessRefresh(const Region& req, const PixelBuffer* pb,
                                         const RenderedCursor* renderedCursor,
                                         size_t maxUpdateSize)
{
    if (videoDetected)
        return;

    doUpdate(false, getLosslessRefresh(req, maxUpdateSize),
             Region(), Point(), std::vector<CopyPassRect>(), pb, renderedCursor);
}

void EncodeManager::doUpdate(bool allowLossy, const Region& changed_,
                             const Region& copied, const Point& copyDelta,
                             const std::vector<CopyPassRect>& copypassed,
                             const PixelBuffer* pb,
                             const RenderedCursor* renderedCursor)
{
    int nRects;
    Region changed, cursorRegion;
    struct timeval start;

    updates++;
    if (conn->cp.supportsUdp)
      ((network::UdpStream *) conn->getOutStream(conn->cp.supportsUdp))->setFrameNumber(updates);


    // The video resolution may have changed, check it
    if (conn->cp.kasmPassed[ConnParams::KASM_MAX_VIDEO_RESOLUTION])
        updateMaxVideoRes(&maxVideoX, &maxVideoY);

    // The dynamic quality params may have changed
    if (Server::dynamicQualityMax && Server::dynamicQualityMax <= 9 &&
        Server::dynamicQualityMax > Server::dynamicQualityMin) {
      dynamicQualityMin = Server::dynamicQualityMin;
      dynamicQualityOff = Server::dynamicQualityMax - Server::dynamicQualityMin;
    } else if (Server::dynamicQualityMin >= 0) {
      dynamicQualityMin = Server::dynamicQualityMin;
      dynamicQualityOff = 0;
    }

    prepareEncoders(allowLossy);

    changed = changed_;

    gettimeofday(&start, NULL);
    memset(&jpegstats, 0, sizeof(codecstats_t));
    memset(&webpstats, 0, sizeof(codecstats_t));

    if (allowLossy && activeEncoders[encoderFullColour] == encoderTightWEBP) {
        webpFallbackUs = (1000 * 1000 / rfb::Server::frameRate) * (static_cast<double>(Server::webpEncodingTime) / 100.0);
    }

    /*
     * We need to render the cursor seperately as it has its own
     * magical pixel buffer, so split it out from the changed region.
     */
    if (renderedCursor != NULL) {
      cursorRegion = changed.intersect(renderedCursor->getEffectiveRect());
      changed.assign_subtract(renderedCursor->getEffectiveRect());
    }

    if (conn->cp.supportsLastRect)
      nRects = 0xFFFF;
    else {
      nRects = copied.numRects();
      nRects += copypassed.size();
      nRects += computeNumRects(changed);
      nRects += computeNumRects(cursorRegion);

      if (watermarkData)
          nRects++;
    }

    conn->writer()->writeFramebufferUpdateStart(nRects);

    writeCopyRects(copied, copyDelta);
    writeCopyPassRects(copypassed);

    /*
     * We start by searching for solid rects, which are then removed
     * from the changed region.
     */
    if (conn->cp.supportsLastRect && !conn->cp.supportsQOI)
      writeSolidRects(&changed, pb);

    writeRects(changed, pb,
               &start, true);
    if (!videoDetected) // In case detection happened between the calls
      writeRects(cursorRegion, renderedCursor);

    if (watermarkData && conn->sendWatermark()) {
      beforeLength = conn->getOutStream(conn->cp.supportsUdp)->length();

      const Rect rect(0, 0, pb->width(), pb->height());
      TightEncoder *encoder = ((TightEncoder *) encoders[encoderTight]);

      conn->writer()->startRect(rect, encoder->encoding);
      encoder->writeWatermarkRect(watermarkData, watermarkDataLen,
                                  watermarkInfo.r,
                                  watermarkInfo.g,
                                  watermarkInfo.b,
                                  watermarkInfo.a);
      conn->writer()->endRect();

      watermarkStats += conn->getOutStream(conn->cp.supportsUdp)->length() - beforeLength;
    }

    updateQualities();

    conn->writer()->writeFramebufferUpdateEnd();
}

void EncodeManager::prepareEncoders(bool allowLossy)
{
  enum EncoderClass solid, bitmap, bitmapRLE;
  enum EncoderClass indexed, indexedRLE, fullColour;

  rdr::S32 preferred;

  std::vector<int>::iterator iter;

  solid = bitmap = bitmapRLE = encoderRaw;
  indexed = indexedRLE = fullColour = encoderRaw;

  // Try to respect the client's wishes
  preferred = conn->getPreferredEncoding();
  switch (preferred) {
  case encodingRRE:
    // Horrible for anything high frequency and/or lots of colours
    bitmapRLE = indexedRLE = encoderRRE;
    break;
  case encodingHextile:
    // Slightly less horrible
    bitmapRLE = indexedRLE = fullColour = encoderHextile;
    break;
  case encodingTight:
    if (encoders[encoderTightQOI]->isSupported() &&
        (conn->cp.pf().bpp >= 16))
      fullColour = encoderTightQOI;
    else if (encoders[encoderTightWEBP]->isSupported() &&
        (conn->cp.pf().bpp >= 16) && allowLossy)
      fullColour = encoderTightWEBP;
    else if (encoders[encoderTightJPEG]->isSupported() &&
        (conn->cp.pf().bpp >= 16) && allowLossy)
      fullColour = encoderTightJPEG;
    else
      fullColour = encoderTight;
    indexed = indexedRLE = encoderTight;
    bitmap = bitmapRLE = encoderTight;
    break;
  case encodingZRLE:
    fullColour = encoderZRLE;
    bitmapRLE = indexedRLE = encoderZRLE;
    bitmap = indexed = encoderZRLE;
    break;
  }

  // Any encoders still unassigned?

  if (fullColour == encoderRaw) {
    if (encoders[encoderTightQOI]->isSupported() &&
        (conn->cp.pf().bpp >= 16))
      fullColour = encoderTightQOI;
    else if (encoders[encoderTightWEBP]->isSupported() &&
        (conn->cp.pf().bpp >= 16) && allowLossy)
      fullColour = encoderTightWEBP;
    else if (encoders[encoderTightJPEG]->isSupported() &&
        (conn->cp.pf().bpp >= 16) && allowLossy)
      fullColour = encoderTightJPEG;
    else if (encoders[encoderZRLE]->isSupported())
      fullColour = encoderZRLE;
    else if (encoders[encoderTight]->isSupported())
      fullColour = encoderTight;
    else if (encoders[encoderHextile]->isSupported())
      fullColour = encoderHextile;
  }

  if (indexed == encoderRaw) {
    if (encoders[encoderZRLE]->isSupported())
      indexed = encoderZRLE;
    else if (encoders[encoderTight]->isSupported())
      indexed = encoderTight;
    else if (encoders[encoderHextile]->isSupported())
      indexed = encoderHextile;
  }

  if (indexedRLE == encoderRaw)
    indexedRLE = indexed;

  if (bitmap == encoderRaw)
    bitmap = indexed;
  if (bitmapRLE == encoderRaw)
    bitmapRLE = bitmap;

  if (solid == encoderRaw) {
    if (encoders[encoderTight]->isSupported())
      solid = encoderTight;
    else if (encoders[encoderRRE]->isSupported())
      solid = encoderRRE;
    else if (encoders[encoderZRLE]->isSupported())
      solid = encoderZRLE;
    else if (encoders[encoderHextile]->isSupported())
      solid = encoderHextile;
  }

  // JPEG is the only encoder that can reduce things to grayscale
  if ((conn->cp.subsampling == subsampleGray) &&
      encoders[encoderTightJPEG]->isSupported() && allowLossy) {
    solid = bitmap = bitmapRLE = encoderTightJPEG;
    indexed = indexedRLE = fullColour = encoderTightJPEG;
  }

  activeEncoders[encoderSolid] = solid;
  activeEncoders[encoderBitmap] = bitmap;
  activeEncoders[encoderBitmapRLE] = bitmapRLE;
  activeEncoders[encoderIndexed] = indexed;
  activeEncoders[encoderIndexedRLE] = indexedRLE;
  activeEncoders[encoderFullColour] = fullColour;

  for (iter = activeEncoders.begin(); iter != activeEncoders.end(); ++iter) {
    Encoder *encoder;

    encoder = encoders[*iter];

    encoder->setCompressLevel(conn->cp.compressLevel);
    encoder->setQualityLevel(conn->cp.qualityLevel);
    encoder->setFineQualityLevel(conn->cp.fineQualityLevel,
                                 conn->cp.subsampling);
  }
}

Region EncodeManager::getLosslessRefresh(const Region& req,
                                         size_t maxUpdateSize)
{
  std::vector<Rect> rects;
  Region refresh;
  size_t area;

  // We make a conservative guess at the compression ratio at 2:1
  maxUpdateSize *= 2;

  area = 0;
  lossyRegion.intersect(req).get_rects(&rects);
  while (!rects.empty()) {
    size_t idx;
    Rect rect;

    // Grab a random rect so we don't keep damaging and restoring the
    // same rect over and over
    idx = rand() % rects.size();

    rect = rects[idx];

    // Add rects until we exceed the threshold, then include as much as
    // possible of the final rect
    if ((area + rect.area()) > maxUpdateSize) {
      // Use the narrowest axis to avoid getting to thin rects
      if (rect.width() > rect.height()) {
        int width = (maxUpdateSize - area) / rect.height();
        rect.br.x = rect.tl.x + __rfbmax(1, width);
      } else {
        int height = (maxUpdateSize - area) / rect.width();
        rect.br.y = rect.tl.y + __rfbmax(1, height);
      }
      refresh.assign_union(Region(rect));
      break;
    }

    area += rect.area();
    refresh.assign_union(Region(rect));

    rects.erase(rects.begin() + idx);
  }

  return refresh;
}

int EncodeManager::computeNumRects(const Region& changed)
{
  int numRects;
  std::vector<Rect> rects;
  std::vector<Rect>::const_iterator rect;

  numRects = 0;
  changed.get_rects(&rects);
  for (rect = rects.begin(); rect != rects.end(); ++rect) {
    int w, h, sw, sh;

    w = rect->width();
    h = rect->height();

    // No split necessary?
    if ((((w*h) < SubRectMaxArea) && (w < SubRectMaxWidth)) ||
        (videoDetected && !encoders[encoderTightWEBP]->isSupported())) {
      numRects += 1;
      continue;
    }

    if (w <= SubRectMaxWidth)
      sw = w;
    else
      sw = SubRectMaxWidth;

    sh = SubRectMaxArea / sw;

    // ceil(w/sw) * ceil(h/sh)
    numRects += (((w - 1)/sw) + 1) * (((h - 1)/sh) + 1);
  }

  return numRects;
}

Encoder *EncodeManager::startRect(const Rect& rect, int type, const bool trackQuality,
                                  const uint8_t isWebp)
{
  Encoder *encoder;
  int klass, equiv;

  activeType = type;
  klass = activeEncoders[activeType];
  if (isWebp)
    klass = encoderTightWEBP;

  beforeLength = conn->getOutStream(conn->cp.supportsUdp)->length();

  stats[klass][activeType].rects++;
  stats[klass][activeType].pixels += rect.area();
  equiv = 12 + rect.area() * (conn->cp.pf().bpp/8);
  stats[klass][activeType].equivalent += equiv;

  encoder = encoders[klass];
  conn->writer()->startRect(rect, encoder->encoding);

  if (type == encoderFullColour && dynamicQualityMin > -1 && trackQuality) {
    trackRectQuality(rect);

    // Set the dynamic quality here. Unset fine quality, as it would overrule us
    encoder->setQualityLevel(scaledQuality(rect));
    encoder->setFineQualityLevel(-1, subsampleUndefined);
  }

  if (encoder->flags & EncoderLossy && (!encoder->treatLossless() || videoDetected))
    lossyRegion.assign_union(Region(rect));
  else
    lossyRegion.assign_subtract(Region(rect));

  return encoder;
}

void EncodeManager::endRect(const uint8_t isWebp)
{
  int klass;
  int length;

  conn->writer()->endRect();

  length = conn->getOutStream(conn->cp.supportsUdp)->length() - beforeLength;

  klass = activeEncoders[activeType];
  if (isWebp)
    klass = encoderTightWEBP;
  stats[klass][activeType].bytes += length;
}

void EncodeManager::writeCopyPassRects(const std::vector<CopyPassRect>& copypassed)
{
  std::vector<CopyPassRect>::const_iterator rect;

  Region lossyCopy;

  beforeLength = conn->getOutStream(conn->cp.supportsUdp)->length();

  for (rect = copypassed.begin(); rect != copypassed.end(); ++rect) {
    int equiv;
    const Region tmp(rect->rect);

    copyStats.rects++;
    copyStats.pixels += rect->rect.area();
    equiv = 12 + rect->rect.area() * (conn->cp.pf().bpp/8);
    copyStats.equivalent += equiv;

    conn->writer()->writeCopyRect(rect->rect, rect->src_x,
                                   rect->src_y);

    lossyCopy = lossyRegion;
    lossyCopy.translate(Point(rect->rect.tl.x - rect->src_x, rect->rect.tl.y - rect->src_y));
    lossyCopy.assign_intersect(tmp);
    lossyRegion.assign_union(lossyCopy);
  }

  copyStats.bytes += conn->getOutStream(conn->cp.supportsUdp)->length() - beforeLength;
}

void EncodeManager::writeCopyRects(const Region& copied, const Point& delta)
{
  std::vector<Rect> rects;
  std::vector<Rect>::const_iterator rect;

  Region lossyCopy;

  beforeLength = conn->getOutStream(conn->cp.supportsUdp)->length();

  copied.get_rects(&rects, delta.x <= 0, delta.y <= 0);
  for (rect = rects.begin(); rect != rects.end(); ++rect) {
    int equiv;

    copyStats.rects++;
    copyStats.pixels += rect->area();
    equiv = 12 + rect->area() * (conn->cp.pf().bpp/8);
    copyStats.equivalent += equiv;

    conn->writer()->writeCopyRect(*rect, rect->tl.x - delta.x,
                                   rect->tl.y - delta.y);
  }

  copyStats.bytes += conn->getOutStream(conn->cp.supportsUdp)->length() - beforeLength;

  lossyCopy = lossyRegion;
  lossyCopy.translate(delta);
  lossyCopy.assign_intersect(copied);
  lossyRegion.assign_union(lossyCopy);
}

void EncodeManager::writeSolidRects(Region *changed, const PixelBuffer* pb)
{
  std::vector<Rect> rects;
  std::vector<Rect>::const_iterator rect;

  changed->get_rects(&rects);
  for (rect = rects.begin(); rect != rects.end(); ++rect)
    findSolidRect(*rect, changed, pb);
}

void EncodeManager::findSolidRect(const Rect& rect, Region *changed,
                                  const PixelBuffer* pb)
{
  Rect sr;
  int dx, dy, dw, dh;

  // We start by finding a solid 16x16 block
  for (dy = rect.tl.y; dy < rect.br.y; dy += SolidSearchBlock) {

    dh = SolidSearchBlock;
    if (dy + dh > rect.br.y)
      dh = rect.br.y - dy;

    for (dx = rect.tl.x; dx < rect.br.x; dx += SolidSearchBlock) {
      // We define it like this to guarantee alignment
      rdr::U32 _buffer;
      rdr::U8* colourValue = (rdr::U8*)&_buffer;

      dw = SolidSearchBlock;
      if (dx + dw > rect.br.x)
        dw = rect.br.x - dx;

      pb->getImage(colourValue, Rect(dx, dy, dx+1, dy+1));

      sr.setXYWH(dx, dy, dw, dh);
      if (checkSolidTile(sr, colourValue, pb)) {
        Rect erb, erp;

        Encoder *encoder;

        // We then try extending the area by adding more blocks
        // in both directions and pick the combination that gives
        // the largest area.
        sr.setXYWH(dx, dy, rect.br.x - dx, rect.br.y - dy);
        extendSolidAreaByBlock(sr, colourValue, pb, &erb);

        // Did we end up getting the entire rectangle?
        if (erb.equals(rect))
          erp = erb;
        else {
          // Don't bother with sending tiny rectangles
          if (erb.area() < SolidBlockMinArea)
            continue;

          // Extend the area again, but this time one pixel
          // row/column at a time.
          extendSolidAreaByPixel(rect, erb, colourValue, pb, &erp);
        }

        // Send solid-color rectangle.
        encoder = startRect(erp, encoderSolid);
        if (encoder->flags & EncoderUseNativePF) {
          encoder->writeSolidRect(erp.width(), erp.height(),
                                  pb->getPF(), colourValue);
        } else {
          rdr::U32 _buffer2;
          rdr::U8* converted = (rdr::U8*)&_buffer2;

          conn->cp.pf().bufferFromBuffer(converted, pb->getPF(),
                                         colourValue, 1);

          encoder->writeSolidRect(erp.width(), erp.height(),
                                  conn->cp.pf(), converted);
        }
        endRect();

        changed->assign_subtract(Region(erp));

        // Search remaining areas by recursion
        // FIXME: Is this the best way to divide things up?

        // Left? (Note that we've already searched a SolidSearchBlock
        //        pixels high strip here)
        if ((erp.tl.x != rect.tl.x) && (erp.height() > SolidSearchBlock)) {
          sr.setXYWH(rect.tl.x, erp.tl.y + SolidSearchBlock,
                     erp.tl.x - rect.tl.x, erp.height() - SolidSearchBlock);
          findSolidRect(sr, changed, pb);
        }

        // Right?
        if (erp.br.x != rect.br.x) {
          sr.setXYWH(erp.br.x, erp.tl.y, rect.br.x - erp.br.x, erp.height());
          findSolidRect(sr, changed, pb);
        }

        // Below?
        if (erp.br.y != rect.br.y) {
          sr.setXYWH(rect.tl.x, erp.br.y, rect.width(), rect.br.y - erp.br.y);
          findSolidRect(sr, changed, pb);
        }

        return;
      }
    }
  }
}

void EncodeManager::checkWebpFallback(const timeval *start) {
    // Have we taken too long for the frame? If so, drop from WEBP to JPEG
    if (start && activeEncoders[encoderFullColour] == encoderTightWEBP && !webpTookTooLong.load(std::memory_order_relaxed)) {
        const auto us = msSince(start) * 1000;
        if (us > webpFallbackUs)
            webpTookTooLong.store(true, std::memory_order_relaxed);
    }
}

bool EncodeManager::handleTimeout(Timer* t)
{
  if (t == &videoTimer) {
    videoDetected = false;

    unsigned videoTime = rfb::Server::videoTime;
    if (videoTime < 1) videoTime = 1;
    memset(areaPercentages, 0, videoTime * rfb::Server::frameRate);

    // Mark the entire screen as changed, so that scaled parts get refreshed
    // Note: different from the lossless area. That already queues an update,
    // but it happens only after an idle period. This queues a lossy update
    // immediately, which is important if an animated element keeps the screen
    // active, preventing the lossless update.
    conn->add_changed_all();
  }
  return false; // stop the timer
}

void EncodeManager::updateVideoStats(const std::vector<Rect> &rects, const PixelBuffer* pb)
{
  std::vector<Rect>::const_iterator rect;
  uint32_t i;

  if (!rfb::Server::videoTime) {
    videoDetected = true;
    return;
  }

  unsigned area = 0;
  const unsigned samples = rfb::Server::videoTime * rfb::Server::frameRate;
  for (rect = rects.begin(); rect != rects.end(); ++rect) {
    area += rect->area();
  }
  area *= 100;
  area /= pb->getRect().width() * pb->getRect().height();

  areaPercentages[areaCur] = area;
  areaCur++;
  areaCur %= samples;

  area = 0;
  for (i = 0; i < samples; i++)
    area += areaPercentages[i];
  area /= samples;

  if (rfb::Server::printVideoArea)
    vlog.info("Video area %u%%, current threshold for video mode %u%%",
              area, (unsigned) rfb::Server::videoArea);

  if (area > (unsigned) rfb::Server::videoArea) {
    // Initiate low-quality video mode
    videoDetected = true;
    videoTimer.start(1000 * rfb::Server::videoOutTime);
  }
}

PixelBuffer *rfb::nearestScale(const PixelBuffer *pb, const uint16_t w, const uint16_t h,
                                 const float diff)
{
  ManagedPixelBuffer *newpb = new ManagedPixelBuffer(pb->getPF(), w, h);
  uint16_t x, y;
  int oldstride, newstride;
  const rdr::U8 *oldpxorig = pb->getBuffer(pb->getRect(), &oldstride);
  const rdr::U8 *oldpx;
  rdr::U8 *newpx = newpb->getBufferRW(newpb->getRect(), &newstride);
  const uint16_t bpp = pb->getPF().bpp / 8;
  const float rowstep = 1 / diff;

  for (y = 0; y < h; y++) {
    const uint16_t ny = rowstep * y;
    oldpx = oldpxorig + oldstride * bpp * ny;
    for (x = 0; x < w; x++) {
      const uint16_t newx = x / diff;
      memcpy(&newpx[x * bpp], &oldpx[newx * bpp], bpp);
    }
    newpx += newstride * bpp;
  }

  return newpb;
}

PixelBuffer *rfb::bilinearScale(const PixelBuffer *pb, const uint16_t w, const uint16_t h,
                                 const float diff)
{
  ManagedPixelBuffer *newpb = new ManagedPixelBuffer(pb->getPF(), w, h);
  uint16_t x, y;
  int oldstride, newstride;
  const rdr::U8 *oldpx = pb->getBuffer(pb->getRect(), &oldstride);
  rdr::U8 *newpx = newpb->getBufferRW(newpb->getRect(), &newstride);
  const uint16_t bpp = pb->getPF().bpp / 8;
  const float invdiff = 1 / diff;

  for (y = 0; y < h; y++) {
    const float ny = y * invdiff;
    const uint16_t lowy = ny;
    const uint16_t highy = lowy + 1;
    const uint16_t bot = (ny - lowy) * 256;
    const uint16_t top = 256 - bot;

    const rdr::U8 *lowyptr = oldpx + oldstride * bpp * lowy;
    const rdr::U8 *highyptr = oldpx + oldstride * bpp * highy;

    for (x = 0; x < w; x++) {
      const float nx = x * invdiff;
      const uint16_t lowx = nx;
      const uint16_t highx = lowx + 1;
      const uint16_t right = (nx - lowx) * 256;
      const uint16_t left = 256 - right;

      unsigned i;
      uint32_t val, val2;
      for (i = 0; i < bpp; i++) {
        val = lowyptr[lowx * bpp + i] * left;
        val += lowyptr[highx * bpp + i] * right;
        val >>= 8;

        val2 = highyptr[lowx * bpp + i] * left;
        val2 += highyptr[highx * bpp + i] * right;
        val2 >>= 8;

        newpx[x * bpp + i] = (val * top + val2 * bot) >> 8;
      }
    }
    newpx += newstride * bpp;
  }

  return newpb;
}

PixelBuffer *rfb::progressiveBilinearScale(const PixelBuffer *pb,
                                 const uint16_t tgtw, const uint16_t tgth,
                                 const float tgtdiff)
{
  if (cpu_info::has_sse2) {
    if (tgtdiff >= 0.5f) {
      ManagedPixelBuffer *newpb = new ManagedPixelBuffer(pb->getPF(), tgtw, tgth);

      int oldstride, newstride;
      const rdr::U8 *oldpx = pb->getBuffer(pb->getRect(), &oldstride);
      rdr::U8 *newpx = newpb->getBufferRW(newpb->getRect(), &newstride);

      SSE2_scale(oldpx, tgtw, tgth, newpx, oldstride, newstride, tgtdiff);
      return newpb;
    }

    PixelBuffer *newpb;
    uint16_t neww, newh, oldw, oldh;
    bool del = false;

    do {
      oldw = pb->getRect().width();
      oldh = pb->getRect().height();
      neww = oldw / 2;
      newh = oldh / 2;

      newpb = new ManagedPixelBuffer(pb->getPF(), neww, newh);

      int oldstride, newstride;
      const rdr::U8 *oldpx = pb->getBuffer(pb->getRect(), &oldstride);
      rdr::U8 *newpx = ((ManagedPixelBuffer *) newpb)->getBufferRW(newpb->getRect(),
                                                                   &newstride);

      SSE2_halve(oldpx, neww, newh, newpx, oldstride, newstride);

      if (del)
        delete pb;
      del = true;

      pb = newpb;
    } while (tgtw * 2 < neww);

    // Final, non-halving step
    if (tgtw != neww || tgth != newh) {
      oldw = pb->getRect().width();
      oldh = pb->getRect().height();

      newpb = new ManagedPixelBuffer(pb->getPF(), tgtw, tgth);

      int oldstride, newstride;
      const rdr::U8 *oldpx = pb->getBuffer(pb->getRect(), &oldstride);
      rdr::U8 *newpx = ((ManagedPixelBuffer *) newpb)->getBufferRW(newpb->getRect(),
                                                                   &newstride);

      SSE2_scale(oldpx, tgtw, tgth, newpx, oldstride, newstride, tgtw / (float) oldw);
      if (del)
        delete pb;
    }

    return newpb;
  } // SSE2

  if (tgtdiff >= 0.5f)
    return bilinearScale(pb, tgtw, tgth, tgtdiff);

  PixelBuffer *newpb;
  uint16_t neww, newh, oldw, oldh;
  bool del = false;

  do {
    oldw = pb->getRect().width();
    oldh = pb->getRect().height();
    neww = oldw / 2;
    newh = oldh / 2;

    newpb = bilinearScale(pb, neww, newh, 0.5f);
    if (del)
      delete pb;
    del = true;

    pb = newpb;
  } while (tgtw * 2 < neww);

  // Final, non-halving step
  if (tgtw != neww || tgth != newh) {
    oldw = pb->getRect().width();
    oldh = pb->getRect().height();

    newpb = bilinearScale(pb, tgtw, tgth, tgtw / (float) oldw);
    if (del)
      delete pb;
  }

  return newpb;
}

void EncodeManager::writeRects(const Region& changed, const PixelBuffer* pb,
                               const struct timeval *start,
                               const bool mainScreen)
{
  std::vector<Rect> rects, subrects, scaledrects;
  std::vector<uint8_t> encoderTypes;
  std::vector<uint8_t> isWebp, fromCache;
  std::vector<Palette> palettes;
  std::vector<std::vector<uint8_t> > compresseds;
  std::vector<uint32_t> ms;

  webpTookTooLong.store(false, std::memory_order_relaxed);
  changed.get_rects(&rects);

  // Update stats
  if (mainScreen) {
    updateVideoStats(rects, pb);
  }

  if (videoDetected) {
    rects.clear();
    rects.push_back(pb->getRect());
  }

  subrects.reserve(rects.size() * 1.5f);

  for (const auto& rect : rects) {
    int sw, sh;
    Rect sr;

    const auto w = rect.width();
    const auto h = rect.height();

    // No split necessary?
    if ((((w*h) < SubRectMaxArea) && (w < SubRectMaxWidth)) ||
        (videoDetected && !encoders[encoderTightWEBP]->isSupported())) {
      subrects.push_back(rect);
      trackRectQuality(rect);
      continue;
    }

    if (w <= SubRectMaxWidth)
      sw = w;
    else
      sw = SubRectMaxWidth;

    sh = SubRectMaxArea / sw;

    for (sr.tl.y = rect.tl.y; sr.tl.y < rect.br.y; sr.tl.y += sh) {
      sr.br.y = sr.tl.y + sh;
      if (sr.br.y > rect.br.y)
        sr.br.y = rect.br.y;

      for (sr.tl.x = rect.tl.x; sr.tl.x < rect.br.x; sr.tl.x += sw) {
        sr.br.x = sr.tl.x + sw;
        if (sr.br.x > rect.br.x)
          sr.br.x = rect.br.x;

        subrects.push_back(sr);
        trackRectQuality(sr);
      }
    }
  }

  const size_t subrects_size = subrects.size();

  encoderTypes.resize(subrects_size);
  isWebp.resize(subrects_size);
  fromCache.resize(subrects_size);
  palettes.resize(subrects_size);
  compresseds.resize(subrects_size);
  scaledrects.resize(subrects_size);
  ms.resize(subrects_size);

  // In case the current resolution is above the max video res, and video was detected,
  // scale to that res, keeping aspect ratio
  struct timeval scalestart;
  gettimeofday(&scalestart, NULL);

  const PixelBuffer *scaledpb = NULL;
  if (videoDetected &&
      (maxVideoX < pb->getRect().width() || maxVideoY < pb->getRect().height())) {
    const float xdiff = maxVideoX / (float) pb->getRect().width();
    const float ydiff = maxVideoY / (float) pb->getRect().height();

    const float diff = xdiff < ydiff ? xdiff : ydiff;

    const uint16_t neww = pb->getRect().width() * diff;
    const uint16_t newh = pb->getRect().height() * diff;
    switch (Server::videoScaling) {
      case 0:
        scaledpb = nearestScale(pb, neww, newh,
                      diff);
      break;
      case 1:
        scaledpb = bilinearScale(pb, neww, newh,
                      diff);
      break;
      case 2:
        scaledpb = progressiveBilinearScale(pb, neww, newh,
                      diff);
      break;
    }

    for (uint32_t i = 0; i < subrects_size; ++i) {
      const Rect old = scaledrects[i] = subrects[i];
      scaledrects[i].br.x *= diff;
      scaledrects[i].br.y *= diff;
      scaledrects[i].tl.x *= diff;
      scaledrects[i].tl.y *= diff;

      // Make sure everything is at least one pixel still
      if (old.br.x != old.tl.x && scaledrects[i].br.x == scaledrects[i].tl.x) {
        if (scaledrects[i].br.x < neww - 1)
          scaledrects[i].br.x++;
        else
          scaledrects[i].tl.x--;
      }

      if (old.br.y != old.tl.y && scaledrects[i].br.y == scaledrects[i].tl.y) {
        if (scaledrects[i].br.y < newh - 1)
          scaledrects[i].br.y++;
        else
          scaledrects[i].tl.y--;
      }
    }
  }
  scalingTime = msSince(&scalestart);

    arena.execute([&] {
        tbb::parallel_for(static_cast<size_t>(0), subrects_size, [&](size_t i) {
            encoderTypes[i] = getEncoderType(subrects[i], pb, &palettes[i], compresseds[i],
                        &isWebp[i], &fromCache[i],
                        scaledpb, scaledrects[i], ms[i]);
            checkWebpFallback(start);
        });
    });

  for (uint32_t i = 0; i < subrects_size; ++i) {
    if (encoderTypes[i] == encoderFullColour) {
      if (isWebp[i])
        webpstats.ms += ms[i];
      else
        jpegstats.ms += ms[i]; // Also covers QOI for now
    }
  }

  if (start) {
    encodingTime = msSince(start);

    if (vlog.getLevel() >= rfb::LogWriter::LEVEL_DEBUG) {
      framesSinceEncPrint++;
      if (maxEncodingTime < encodingTime)
        maxEncodingTime = encodingTime;

      if (framesSinceEncPrint >= rfb::Server::frameRate) {
        vlog.info("Max encoding time during the last %u frames: %u ms (limit %u, near limit %.0f)",
                  framesSinceEncPrint, maxEncodingTime, 1000/rfb::Server::frameRate,
                  1000/rfb::Server::frameRate * 0.8f);
        maxEncodingTime = 0;
        framesSinceEncPrint = 0;
      }
    }
  }

  if (webpTookTooLong.load(std::memory_order_relaxed))
    activeEncoders[encoderFullColour] = encoderTightJPEG;

  for (uint32_t i = 0; i < subrects_size; ++i) {
    if (encCache->enabled && !compresseds[i].empty() && !fromCache[i] &&
        !encoders[encoderTightQOI]->isSupported()) {
      void *tmp = malloc(compresseds[i].size());
      memcpy(tmp, &compresseds[i][0], compresseds[i].size());
      encCache->add(isWebp[i] ? encoderTightWEBP : encoderTightJPEG,
                    subrects[i].tl.x, subrects[i].tl.y, subrects[i].width(), subrects[i].height(),
                    compresseds[i].size(), tmp);
    }

    writeSubRect(subrects[i], pb, encoderTypes[i], palettes[i], compresseds[i], isWebp[i]);
  }

  if (scaledpb)
    delete scaledpb;
}

uint8_t EncodeManager::getEncoderType(const Rect& rect, const PixelBuffer *pb,
                                      Palette *pal, std::vector<uint8_t> &compressed,
                                      uint8_t *isWebp, uint8_t *fromCache,
                                      const PixelBuffer *scaledpb, const Rect& scaledrect,
                                      uint32_t &ms) const
{
  struct RectInfo info;
  unsigned int maxColours = 256;
  PixelBuffer *ppb;
  Encoder *encoder;

  bool useRLE;
  EncoderType type;

  encoder = encoders[activeEncoders[encoderIndexedRLE]];
  if (maxColours > encoder->maxPaletteSize)
    maxColours = encoder->maxPaletteSize;
  encoder = encoders[activeEncoders[encoderIndexed]];
  if (maxColours > encoder->maxPaletteSize)
    maxColours = encoder->maxPaletteSize;

  ppb = preparePixelBuffer(rect, pb, true);
  info.palette = pal;

  if (!analyseRect(ppb, &info, maxColours))
    info.palette->clear();

  // Different encoders might have different RLE overhead, but
  // here we do a guess at RLE being the better choice if reduces
  // the pixel count by 50%.
  useRLE = info.rleRuns <= (rect.area() * 2);

  switch (info.palette->size()) {
  case 0:
    type = encoderFullColour;
    break;
  case 1:
    type = encoderSolid;
    break;
  case 2:
    if (useRLE)
      type = encoderBitmapRLE;
    else
      type = encoderBitmap;
    break;
  default:
    if (useRLE)
      type = encoderIndexedRLE;
    else
      type = encoderIndexed;
  }

  if (scaledpb || conn->cp.supportsQOI)
    type = encoderFullColour;

  *isWebp = 0;
  *fromCache = 0;
  ms = 0;
  if (type == encoderFullColour) {
    uint32_t len;
    const void *data;
    struct timeval start;
    gettimeofday(&start, NULL);

    if (encCache->enabled &&
        (data = encCache->get(activeEncoders[encoderFullColour],
                              rect.tl.x, rect.tl.y, rect.width(), rect.height(),
                              len))) {
      compressed.resize(len);
      memcpy(&compressed[0], data, len);
      *fromCache = 1;
    } else if (activeEncoders[encoderFullColour] == encoderTightWEBP && !webpTookTooLong) {
      if (scaledpb) {
        delete ppb;
        ppb = preparePixelBuffer(scaledrect, scaledpb,
                                 encoders[encoderTightWEBP]->flags & EncoderUseNativePF ?
                                 false : true);
      } else if (encoders[encoderTightWEBP]->flags & EncoderUseNativePF) {
        delete ppb;
        ppb = preparePixelBuffer(rect, pb, false);
      }

      ((TightWEBPEncoder *) encoders[encoderTightWEBP])->compressOnly(ppb,
                                                                      scaledQuality(rect),
                                                                      compressed,
                                                                      videoDetected);
      *isWebp = 1;
    } else if (activeEncoders[encoderFullColour] == encoderTightQOI) {
      if (scaledpb) {
        delete ppb;
        ppb = preparePixelBuffer(scaledrect, scaledpb,
                                 encoders[encoderTightQOI]->flags & EncoderUseNativePF ?
                                 false : true);
      } else if (encoders[encoderTightQOI]->flags & EncoderUseNativePF) {
        delete ppb;
        ppb = preparePixelBuffer(rect, pb, false);
      }

      ((TightQOIEncoder *) encoders[encoderTightQOI])->compressOnly(ppb,
                                                                      scaledQuality(rect),
                                                                      compressed,
                                                                      videoDetected);
    } else if (activeEncoders[encoderFullColour] == encoderTightJPEG || webpTookTooLong) {
      if (scaledpb) {
        delete ppb;
        ppb = preparePixelBuffer(scaledrect, scaledpb,
                                 encoders[encoderTightJPEG]->flags & EncoderUseNativePF ?
                                 false : true);
      } else if (encoders[encoderTightJPEG]->flags & EncoderUseNativePF) {
        delete ppb;
        ppb = preparePixelBuffer(rect, pb, false);
      }

      ((TightJPEGEncoder *) encoders[encoderTightJPEG])->compressOnly(ppb,
                                                                      scaledQuality(rect),
                                                                      compressed,
                                                                      videoDetected);
    }

    ms = msSince(&start);
  }

  delete ppb;

  return type;
}

void EncodeManager::writeSubRect(const Rect& rect, const PixelBuffer *pb,
                                 const uint8_t type, const Palette &pal,
                                 const std::vector<uint8_t> &compressed,
                                 const uint8_t isWebp)
{
  PixelBuffer *ppb;
  Encoder *encoder;

  encoder = startRect(rect, type, compressed.size() == 0, isWebp);

  if (compressed.size()) {
    if (isWebp) {
      ((TightWEBPEncoder *) encoder)->writeOnly(compressed);
      webpstats.area += rect.area();
      webpstats.rects++;
    } else if (encoders[encoderTightQOI]->isSupported()) {
      ((TightQOIEncoder *) encoder)->writeOnly(compressed);
      jpegstats.area += rect.area(); // Also QOI for now
      jpegstats.rects++;
    } else {
      ((TightJPEGEncoder *) encoder)->writeOnly(compressed);
      jpegstats.area += rect.area();
      jpegstats.rects++;
    }
  } else {
    if (encoder->flags & EncoderUseNativePF) {
      ppb = preparePixelBuffer(rect, pb, false);
    } else {
      ppb = preparePixelBuffer(rect, pb, true);
    }

    encoder->writeRect(ppb, pal);
    delete ppb;
  }

  endRect(isWebp);
}

bool EncodeManager::checkSolidTile(const Rect& r, const rdr::U8* colourValue,
                                   const PixelBuffer *pb)
{
  switch (pb->getPF().bpp) {
  case 32:
    return checkSolidTile(r, *(const rdr::U32*)colourValue, pb);
  case 16:
    return checkSolidTile(r, *(const rdr::U16*)colourValue, pb);
  default:
    return checkSolidTile(r, *(const rdr::U8*)colourValue, pb);
  }
}

void EncodeManager::extendSolidAreaByBlock(const Rect& r,
                                           const rdr::U8* colourValue,
                                           const PixelBuffer *pb, Rect* er)
{
  int dx, dy, dw, dh;
  int w_prev;
  Rect sr;
  int w_best = 0, h_best = 0;

  w_prev = r.width();

  // We search width first, back off when we hit a different colour,
  // and restart with a larger height. We keep track of the
  // width/height combination that gives us the largest area.
  for (dy = r.tl.y; dy < r.br.y; dy += SolidSearchBlock) {

    dh = SolidSearchBlock;
    if (dy + dh > r.br.y)
      dh = r.br.y - dy;

    // We test one block here outside the x loop in order to break
    // the y loop right away.
    dw = SolidSearchBlock;
    if (dw > w_prev)
      dw = w_prev;

    sr.setXYWH(r.tl.x, dy, dw, dh);
    if (!checkSolidTile(sr, colourValue, pb))
      break;

    for (dx = r.tl.x + dw; dx < r.tl.x + w_prev;) {

      dw = SolidSearchBlock;
      if (dx + dw > r.tl.x + w_prev)
        dw = r.tl.x + w_prev - dx;

      sr.setXYWH(dx, dy, dw, dh);
      if (!checkSolidTile(sr, colourValue, pb))
        break;

      dx += dw;
    }

    w_prev = dx - r.tl.x;
    if (w_prev * (dy + dh - r.tl.y) > w_best * h_best) {
      w_best = w_prev;
      h_best = dy + dh - r.tl.y;
    }
  }

  er->tl.x = r.tl.x;
  er->tl.y = r.tl.y;
  er->br.x = er->tl.x + w_best;
  er->br.y = er->tl.y + h_best;
}

void EncodeManager::extendSolidAreaByPixel(const Rect& r, const Rect& sr,
                                           const rdr::U8* colourValue,
                                           const PixelBuffer *pb, Rect* er)
{
  int cx, cy;
  Rect tr;

  // Try to extend the area upwards.
  for (cy = sr.tl.y - 1; cy >= r.tl.y; cy--) {
    tr.setXYWH(sr.tl.x, cy, sr.width(), 1);
    if (!checkSolidTile(tr, colourValue, pb))
      break;
  }
  er->tl.y = cy + 1;

  // ... downwards.
  for (cy = sr.br.y; cy < r.br.y; cy++) {
    tr.setXYWH(sr.tl.x, cy, sr.width(), 1);
    if (!checkSolidTile(tr, colourValue, pb))
      break;
  }
  er->br.y = cy;

  // ... to the left.
  for (cx = sr.tl.x - 1; cx >= r.tl.x; cx--) {
    tr.setXYWH(cx, er->tl.y, 1, er->height());
    if (!checkSolidTile(tr, colourValue, pb))
      break;
  }
  er->tl.x = cx + 1;

  // ... to the right.
  for (cx = sr.br.x; cx < r.br.x; cx++) {
    tr.setXYWH(cx, er->tl.y, 1, er->height());
    if (!checkSolidTile(tr, colourValue, pb))
      break;
  }
  er->br.x = cx;
}

PixelBuffer* EncodeManager::preparePixelBuffer(const Rect& rect,
                                               const PixelBuffer *pb,
                                               bool convert) const
{
  const rdr::U8* buffer;
  int stride;

  // Do wo need to convert the data?
  if (convert && !conn->cp.pf().equal(pb->getPF())) {
    ManagedPixelBuffer *convertedPixelBuffer = new ManagedPixelBuffer;

    convertedPixelBuffer->setPF(conn->cp.pf());
    convertedPixelBuffer->setSize(rect.width(), rect.height());

    buffer = pb->getBuffer(rect, &stride);
    convertedPixelBuffer->imageRect(pb->getPF(),
                                   convertedPixelBuffer->getRect(),
                                   buffer, stride);

    return convertedPixelBuffer;
  }

  // Otherwise we still need to shift the coordinates. We have our own
  // abusive subclass of FullFramePixelBuffer for this.

  buffer = pb->getBuffer(rect, &stride);

  OffsetPixelBuffer *offsetPixelBuffer = new OffsetPixelBuffer;
  offsetPixelBuffer->update(pb->getPF(), rect.width(), rect.height(),
                           buffer, stride);

  return offsetPixelBuffer;
}

bool EncodeManager::analyseRect(const PixelBuffer *pb,
                                struct RectInfo *info, int maxColours) const
{
  const rdr::U8* buffer;
  int stride;

  buffer = pb->getBuffer(pb->getRect(), &stride);

  switch (pb->getPF().bpp) {
  case 32:
    return analyseRect(pb->width(), pb->height(),
                       (const rdr::U32*)buffer, stride,
                       info, maxColours);
  case 16:
    return analyseRect(pb->width(), pb->height(),
                       (const rdr::U16*)buffer, stride,
                       info, maxColours);
  default:
    return analyseRect(pb->width(), pb->height(),
                       (const rdr::U8*)buffer, stride,
                       info, maxColours);
  }
}

void EncodeManager::OffsetPixelBuffer::update(const PixelFormat& pf,
                                              int width, int height,
                                              const rdr::U8* data_,
                                              int stride_)
{
  format = pf;
  width_ = width;
  height_ = height;
  // Forced cast. We never write anything though, so it should be safe.
  data = (rdr::U8*)data_;
  stride = stride_;
}

rdr::U8* EncodeManager::OffsetPixelBuffer::getBufferRW(const Rect& r, int* stride)
{
  throw rfb::Exception("Invalid write attempt to OffsetPixelBuffer");
}

// Preprocessor generated, optimised methods

#define BPP 8
#include "EncodeManagerBPP.cxx"
#undef BPP
#define BPP 16
#include "EncodeManagerBPP.cxx"
#undef BPP
#define BPP 32
#include "EncodeManagerBPP.cxx"
#undef BPP

// Dynamic quality tracking
void EncodeManager::updateQualities() {
  struct timeval now;
  gettimeofday(&now, NULL);

  // Remove elements that haven't been touched in 5s. Update the scores.
  for (std::list<QualityInfo*>::iterator it = qualityList.begin(); it != qualityList.end(); ) {
    QualityInfo * const cur = *it;
    const unsigned since = msBetween(&cur->lastUpdate, &now);
    if (since > 5000) {
      delete cur;
      it = qualityList.erase(it);
    } else {
      cur->score -= cur->score / 16;
      it++;
    }
  }
}

static bool closeEnough(const Rect& unioned, const int& unionArea,
                        const Rect& check, const int& checkArea) {
  const Point p = unioned.tl.subtract(check.tl);
  if (abs(p.x) > 32 ||
      abs(p.y) > 32)
      return false;

  if (abs(unionArea - checkArea) > 4096)
    return false;

  return true;
}

void EncodeManager::trackRectQuality(const Rect& rect) {

  const int searchArea = rect.area();
  struct timeval now;
  gettimeofday(&now, NULL);

  for (std::list<QualityInfo*>::iterator it = qualityList.begin(); it != qualityList.end(); it++) {
    QualityInfo * const cur = *it;
    const int curArea = cur->rect.area();
    const Rect unioned = cur->rect.union_boundary(rect);
    const int unionArea = unioned.area();
    // Is this close enough to match?
    // e.g. ads that change parts in one frame and more in others
    if (rect.enclosed_by(cur->rect) ||
        cur->rect.enclosed_by(rect) ||
        closeEnough(unioned, unionArea, cur->rect, curArea) ||
        closeEnough(unioned, unionArea, rect, searchArea)) {

        // This existing rect matched. Set it to the larger of the two,
        // and add to its score.
        if (searchArea > curArea)
          cur->rect = rect;

        cur->score += SCORE_INCREMENT;
        cur->lastUpdate = now;
        return;
    }
  }

  // It wasn't found, add it
  QualityInfo *info = new QualityInfo;
  info->rect = rect;
  info->score = 0;
  info->lastUpdate = now;
  qualityList.push_back(info);
}

// Returns the change-tracked quality, 0-128, where 128 is max quality
unsigned EncodeManager::getQuality(const Rect& rect) const {

  const int searchArea = rect.area();

  for (std::list<QualityInfo*>::const_iterator it = qualityList.begin(); it != qualityList.end(); it++) {
    const QualityInfo * const cur = *it;
    const int curArea = cur->rect.area();
    const Rect unioned = cur->rect.union_boundary(rect);
    const int unionArea = unioned.area();
    // Is this close enough to match?
    // e.g. ads that change parts in one frame and more in others
    if (rect.enclosed_by(cur->rect) ||
        cur->rect.enclosed_by(rect) ||
        closeEnough(unioned, unionArea, cur->rect, curArea) ||
        closeEnough(unioned, unionArea, rect, searchArea)) {

        unsigned score = cur->score;
        if (score > 128)
          score = 128;
        score = 128 - score;

        return score;
    }
  }

  return 128; // Not found, this shouldn't happen - return max quality then
}

// Returns the scaled quality, 0-9, where 9 is max
// Optionally takes bandwidth into account
unsigned EncodeManager::scaledQuality(const Rect& rect) const {

  unsigned dynamic;

  dynamic = getQuality(rect);

  // The tracker gives quality as 0-128. Convert to our desired range
  dynamic *= dynamicQualityOff;
  dynamic += 64; // Rounding
  dynamic /= 128;
  dynamic += dynamicQualityMin;

  // Bandwidth adjustment
  if (!Server::preferBandwidth) {
    // Prefer quality, if there's bandwidth available, don't go below 7
    if (curMaxUpdateSize > 2000 && dynamic < 7)
      dynamic = 7;
  }

  return dynamic;
}

void EncodeManager::resetZlib() {
  ((TightEncoder *) encoders[encoderTight])->resetZlib();
}

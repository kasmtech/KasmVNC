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
#ifndef __RFB_ENCODEMANAGER_H__
#define __RFB_ENCODEMANAGER_H__

#include <vector>
#include <list>

#include <rdr/types.h>
#include <rfb/PixelBuffer.h>
#include <rfb/Region.h>
#include <rfb/Timer.h>
#include <rfb/UpdateTracker.h>

#include <stdint.h>
#include <atomic>
#include <tbb/task_arena.h>
#include <sys/time.h>

namespace rfb {
  class SConnection;
  class Encoder;
  class UpdateInfo;
  class Palette;
  class PixelBuffer;
  class RenderedCursor;
  class EncCache;
  struct Rect;

  struct RectInfo;
  struct QualityInfo;

  class EncodeManager: public Timer::Callback {
  public:
    EncodeManager(SConnection* conn, EncCache *encCache);
    ~EncodeManager() override;

    void logStats();

    // Hack to let ConnParams calculate the client's preferred encoding
    static bool supported(int encoding);

    bool needsLosslessRefresh(const Region& req);
    void pruneLosslessRefresh(const Region& limits);

    void writeUpdate(const UpdateInfo& ui, const PixelBuffer* pb,
                     const RenderedCursor* renderedCursor,
                     size_t maxUpdateSize = 2000);

    void writeLosslessRefresh(const Region& req, const PixelBuffer* pb,
                              const RenderedCursor* renderedCursor,
                              size_t maxUpdateSize);

    void clearEncodingTime() {
        encodingTime = 0;
    };

    [[nodiscard]] unsigned getEncodingTime() const {
        return encodingTime;
    };
    [[nodiscard]] unsigned getScalingTime() const {
        return scalingTime;
    };

    void resetZlib();

    struct codecstats_t {
      uint32_t ms;
      uint32_t area;
      uint32_t rects;
    };

    codecstats_t jpegstats, webpstats;

  protected:
    void doUpdate(bool allowLossy, const Region& changed,
                  const Region& copied, const Point& copy_delta,
                  const std::vector<CopyPassRect> &copypassed,
                  const PixelBuffer* pb,
                  const RenderedCursor* renderedCursor);
    void prepareEncoders(bool allowLossy);

    Region getLosslessRefresh(const Region& req, size_t maxUpdateSize);

    int computeNumRects(const Region& changed);

    Encoder *startRect(const Rect& rect, int type, const bool trackQuality = true,
                       const uint8_t isWebp = 0);
    void endRect(const uint8_t isWebp = 0);

    void writeCopyRects(const Region& copied, const Point& delta);
    void writeCopyPassRects(const std::vector<CopyPassRect>& copypassed);
    void writeSolidRects(Region *changed, const PixelBuffer* pb);
    void findSolidRect(const Rect& rect, Region *changed, const PixelBuffer* pb);
    void writeRects(const Region& changed, const PixelBuffer* pb,
                    const struct timeval *start = NULL,
                    const bool mainScreen = false);
    void checkWebpFallback(const struct timeval *start);
    void updateVideoStats(const std::vector<Rect> &rects, const PixelBuffer* pb);

    void writeSubRect(const Rect& rect, const PixelBuffer *pb, const uint8_t type,
                      const Palette& pal, const std::vector<uint8_t> &compressed,
                      const uint8_t isWebp);

    uint8_t getEncoderType(const Rect& rect, const PixelBuffer *pb, Palette *pal,
                           std::vector<uint8_t> &compressed, uint8_t *isWebp,
                           uint8_t *fromCache,
                           const PixelBuffer *scaledpb, const Rect& scaledrect,
                           uint32_t &ms) const;

    bool handleTimeout(Timer* t) override;

    bool checkSolidTile(const Rect& r, const rdr::U8* colourValue,
                        const PixelBuffer *pb);
    void extendSolidAreaByBlock(const Rect& r, const rdr::U8* colourValue,
                                const PixelBuffer *pb, Rect* er);
    void extendSolidAreaByPixel(const Rect& r, const Rect& sr,
                                const rdr::U8* colourValue,
                                const PixelBuffer *pb, Rect* er);

    PixelBuffer* preparePixelBuffer(const Rect& rect,
                                    const PixelBuffer *pb, bool convert) const;

    bool analyseRect(const PixelBuffer *pb,
                     struct RectInfo *info, int maxColours) const;

    void updateQualities();
    void trackRectQuality(const Rect& rect);
    unsigned getQuality(const Rect& rect) const;
    unsigned scaledQuality(const Rect& rect) const;

  protected:
    // Preprocessor generated, optimised methods
    inline bool checkSolidTile(const Rect& r, rdr::U8 colourValue,
                               const PixelBuffer *pb);
    inline bool checkSolidTile(const Rect& r, rdr::U16 colourValue,
                               const PixelBuffer *pb);
    inline bool checkSolidTile(const Rect& r, rdr::U32 colourValue,
                               const PixelBuffer *pb);

    inline bool analyseRect(int width, int height,
                            const rdr::U8* buffer, int stride,
                            struct RectInfo *info, int maxColours) const;
    inline bool analyseRect(int width, int height,
                            const rdr::U16* buffer, int stride,
                            struct RectInfo *info, int maxColours) const;
    inline bool analyseRect(int width, int height,
                            const rdr::U32* buffer, int stride,
                            struct RectInfo *info, int maxColours) const;

  protected:
    SConnection *conn;
    tbb::task_arena arena;

    std::vector<Encoder*> encoders;
    std::vector<int> activeEncoders;

    Region lossyRegion;

    struct EncoderStats {
      unsigned rects;
      unsigned long long bytes;
      unsigned long long pixels;
      unsigned long long equivalent;
    };
    typedef std::vector< std::vector<struct EncoderStats> > StatsVector;

    std::list<QualityInfo*> qualityList;
    int dynamicQualityMin;
    int dynamicQualityOff;

    unsigned char *areaPercentages;
    unsigned areaCur;
    bool videoDetected;
    Timer videoTimer;
    uint16_t maxVideoX, maxVideoY;

    unsigned updates;
    EncoderStats copyStats;
    StatsVector stats;
    unsigned long long watermarkStats;
    int activeType;
    int beforeLength;
    size_t curMaxUpdateSize;
    unsigned webpFallbackUs;
    unsigned webpBenchResult;
    std::atomic<bool> webpTookTooLong{false};
    unsigned encodingTime;
    unsigned maxEncodingTime, framesSinceEncPrint;
    unsigned scalingTime;

    EncCache *encCache;

    class OffsetPixelBuffer : public FullFramePixelBuffer {
    public:
      OffsetPixelBuffer() = default;
      ~OffsetPixelBuffer() override = default;

      void update(const PixelFormat& pf, int width, int height,
                  const rdr::U8* data_, int stride);

    private:
      rdr::U8* getBufferRW(const Rect& r, int* stride) override;
    };
  };

  PixelBuffer *nearestScale(const PixelBuffer *pb, const uint16_t w, const uint16_t h,
                            const float diff);
  PixelBuffer *bilinearScale(const PixelBuffer *pb, const uint16_t w, const uint16_t h,
                            const float diff);
  PixelBuffer *progressiveBilinearScale(const PixelBuffer *pb, const uint16_t w, const uint16_t h,
                            const float diff);
}

#endif

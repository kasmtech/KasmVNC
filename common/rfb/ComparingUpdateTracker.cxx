/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
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
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <zlib.h>
#include <rdr/types.h>
#include <rfb/Exception.h>
#include <rfb/LogWriter.h>
#include <rfb/ServerCore.h>
#include <rfb/ComparingUpdateTracker.h>

#include <rfb/adler32.h>
#include <rfb/xxhash.h>

using namespace rfb;

static LogWriter vlog("ComparingUpdateTracker");

static uint32_t ispow(const uint32_t in) {

	if (in < 2) return 0;

	return !(in & (in - 1));
}

static uint32_t npow(uint32_t in) {

	if (ispow(in)) return in;

	in |= in >> 1;
	in |= in >> 2;
	in |= in >> 4;
	in |= in >> 8;
	in |= in >> 16;

	return in + 1;
}

static uint32_t pow2shift(const uint32_t in) {
	return __builtin_ffs(in) - 1;
}

#define SCROLLBLOCK_SIZE 64
#define NUM_TOTALS (1024 * 256)
#define MAX_CHECKS 8

class scrollHasher_t {
protected:
	struct hashdata_t {
		uint32_t hash;

		bool operator ==(const hashdata_t &other) const {
			return hash == other.hash;
		}

		bool operator <(const hashdata_t &other) const {
			return hash < other.hash;
		}
	};

	struct hasher {
		size_t operator()(const hashdata_t &in) const {
			return in.hash;
		}
	};

	struct match_t {
		uint32_t hash, idx;
	};

	uint_fast32_t w, h, d, lineBytes, blockBytes;
	hashdata_t *hashtable;
	uint_fast32_t hashw, hashAnd, hashShift;
	mutable int_fast16_t lastOffX, lastOffY;

	const uint8_t *olddata;
	uint32_t *totals, *starts, *idxtable, *curs;
public:
	scrollHasher_t(): w(0), h(0), d(0), lineBytes(0), blockBytes(0), hashtable(NULL),
				hashw(0), hashAnd(0), hashShift(0),
				lastOffX(0), lastOffY(0),
				olddata(NULL), totals(NULL), starts(NULL), idxtable(NULL) {

		assert(sizeof(hashdata_t) == sizeof(uint32_t));
	}

	virtual ~scrollHasher_t() {
		free(totals);
		free(starts);
		free(curs);
		free(hashtable);
		free(idxtable);
		free((void *) olddata);
	}

	virtual void calcHashes(const uint8_t *ptr,
			const uint32_t w_, const uint32_t h_, const uint32_t d_) = 0;

	virtual void invalidate(const uint_fast32_t x, uint_fast32_t y, uint_fast32_t h) = 0;

	virtual void findBestMatch(const uint8_t * const ptr, const uint_fast32_t maxLines,
				const uint_fast32_t inx, const uint_fast32_t iny,
				uint_fast32_t *outx,
				uint_fast32_t *outy,
				uint_fast32_t *outlines) const = 0;

	virtual void findBlock(const uint8_t * const ptr,
				const uint_fast32_t inx, const uint_fast32_t iny,
				uint_fast32_t *outx,
				uint_fast32_t *outy,
				uint_fast32_t *outlines) const = 0;
};

class scrollHasher_vert_t: public scrollHasher_t {
public:
	scrollHasher_vert_t(): scrollHasher_t() {

		totals = (uint32_t *) malloc(sizeof(uint32_t) * NUM_TOTALS);
		starts = (uint32_t *) malloc(sizeof(uint32_t) * NUM_TOTALS);
		curs = (uint32_t *) malloc(sizeof(uint32_t) * NUM_TOTALS);
	}

	void calcHashes(const uint8_t *ptr,
			const uint32_t w_, const uint32_t h_, const uint32_t d_) {

		if (w != w_ || h != h_) {
			// Reallocate
			w = w_;
			h = h_;
			d = d_;
			lineBytes = w * d;
			blockBytes = SCROLLBLOCK_SIZE * d;

			hashw = npow(w / SCROLLBLOCK_SIZE);
			hashAnd = hashw - 1;
			hashShift = pow2shift(hashw);

			hashtable = (hashdata_t *) realloc(hashtable,
								hashw * h * sizeof(uint32_t));
			idxtable = (uint32_t *) realloc(idxtable,
								hashw * h * sizeof(uint32_t));

			olddata = (const uint8_t *) realloc((void *) olddata, w * h * d);
		}

		// We need to make a copy, since the comparer incrementally updates its copy
		memcpy((uint8_t *) olddata, ptr, w * h * d);

		//memset(hashtable, 0, hashw * h * sizeof(uint32_t));
		//memset(idxtable, 0, w * h * sizeof(uint32_t));
		memset(totals, 0, NUM_TOTALS * sizeof(uint32_t));
		//memset(starts, 0, NUM_TOTALS * sizeof(uint32_t));
		//memset(curs, 0, NUM_TOTALS * sizeof(uint32_t));

		for (uint_fast32_t y = 0; y < h; y++) {
			const uint8_t *inptr0 = olddata;
			inptr0 += y * lineBytes;
			for (uint_fast32_t x = 0; x < w; x += SCROLLBLOCK_SIZE) {
				if (w - x < SCROLLBLOCK_SIZE)
					break;

				const uint_fast32_t idx = (y << hashShift) + x / SCROLLBLOCK_SIZE;
				hashtable[idx].hash = XXH64(inptr0, blockBytes, 0);
				totals[hashtable[idx].hash % NUM_TOTALS]++;

				inptr0 += blockBytes;
			}
		}

		// calculate number of unique 21-bit hashes
		/*uint_fast32_t uniqHashes = 0;
		for (uint_fast32_t i = 0; i < NUM_TOTALS; i++) {
			if (totals[i])
				uniqHashes++;
		}
		printf("%lu unique hashes\n", uniqHashes);*/

		// Update starting positions
		uint_fast32_t sum = 0;
		for (uint_fast32_t i = 0; i < NUM_TOTALS; i++) {
			if (!totals[i])
				continue;
			starts[i] = curs[i] = sum;
			sum += totals[i];
		}

		// update index table
		const hashdata_t *src = hashtable;
		for (uint_fast32_t y = 0; y < h; y++) {
			uint_fast32_t ybase = (y << hashShift);
			for (uint_fast32_t x = 0; x < w; x += SCROLLBLOCK_SIZE, ybase++) {

				if (w - x < SCROLLBLOCK_SIZE)
					break;

				const uint_fast32_t val = src[x / SCROLLBLOCK_SIZE].hash;
				const uint_fast32_t smallIdx = val % NUM_TOTALS;

				const uint_fast32_t newpos = curs[smallIdx]++;
				// this assert is very heavy, uncomment only for debugging
				//assert(curs[smallIdx] - starts[smallIdx] <= totals[smallIdx]);
				idxtable[newpos] = ybase;
			}
			src += hashw;
		}

		lastOffX = lastOffY = 0;
	}

	void invalidate(const uint_fast32_t x, uint_fast32_t y, uint_fast32_t h) {

		h += y;
		for (; y < h; y++) {
			memset(&hashtable[(y << hashShift) + x / SCROLLBLOCK_SIZE], 0,
				sizeof(uint32_t));
		}
	}

	void findBestMatch(const uint8_t * const ptr, const uint_fast32_t maxLines,
				const uint_fast32_t inx, const uint_fast32_t iny,
				uint_fast32_t *outx,
				uint_fast32_t *outy,
				uint_fast32_t *outlines) const {
		const uint_fast32_t starthash = (uint32_t) XXH64(ptr, blockBytes, 0);
		const uint_fast32_t smallIdx = starthash % NUM_TOTALS;
		match_t matches[MAX_CHECKS];

		*outlines = 0;

		uint_fast32_t i, upto, curidx, curhash, found = 0, inc;
		upto = totals[smallIdx] + starts[smallIdx];
		if (!totals[smallIdx])
			return;

		inc = totals[smallIdx] / 32;
		if (!inc)
			inc = 1;

		//printf("target hash %lx, it has %u matches\n",
		//	starthash, totals[smallIdx]);

		// First, try the last good offset. If this was a scroll,
		// and we have a good offset, it should match almost everything
		const uint_fast16_t tryX = inx + lastOffX;
		const uint_fast16_t tryY = iny + lastOffY;
		if ((lastOffX || lastOffY) &&
			tryX < w - (SCROLLBLOCK_SIZE - 1) &&
			tryY < h - maxLines) {

			//printf("Trying good offset %ld,%ld for in %lu,%lu, try %lu,%lu\n",
			//	lastOffX, lastOffY, inx, iny, tryX, tryY);

			curidx = (tryY << hashShift) + tryX / SCROLLBLOCK_SIZE;
			curhash = hashtable[curidx].hash;
			if (curhash == starthash &&
				memcmp(ptr, &olddata[tryY * lineBytes + tryX * d], blockBytes) == 0) {

				matches[0].hash = curhash;
				matches[0].idx = curidx;
				found++;
			} /*else printf("Nope, hashes %u %lx %lx, mem %u, maxlines %lu\n",
				curhash == starthash, curhash, starthash,
				memcmp(ptr, &olddata[tryY * lineBytes + tryX * d], blockBytes) == 0,
				maxLines);*/
		}

		for (i = starts[smallIdx]; i < upto; i += inc) {
			curidx = idxtable[i];
			curhash = hashtable[curidx].hash;

			if (curhash != starthash)
				continue;

			// Convert to olddata position
			const uint_fast32_t oldy = curidx >> hashShift;
			const uint_fast32_t oldx = curidx & hashAnd;

			if (memcmp(ptr, &olddata[oldy * lineBytes + oldx * blockBytes], blockBytes))
				continue;

			matches[found].hash = curhash;
			matches[found].idx = curidx;
			found++;
			if (found >= MAX_CHECKS)
				break;
		}

		if (!found)
			return;

		//printf("%lu of those were suitable for further checks\n", found);

		// Find best of them
		uint_fast32_t best = 0, bestmatches = 0;
		for (i = 0; i < found; i++) {

			const uint_fast32_t oldy = matches[i].idx >> hashShift;
			const uint_fast32_t oldx = matches[i].idx & hashAnd;

			uint_fast32_t k, bothMaxLines;
			bothMaxLines = maxLines;
			if (bothMaxLines > h - oldy)
				bothMaxLines = h - oldy;
			for (k = 1; k < bothMaxLines; k++) {
/*				curhash = adler32(adler32(0, NULL, 0), ptr + lineBytes * k,
							blockBytes);
				if (curhash != hashtable[matches[i].idx + hashw * k].hash)
					break;*/
				if (!hashtable[matches[i].idx + (k << hashShift)].hash)
					break; // Invalidated
				if (memcmp(ptr + lineBytes * k,
						&olddata[(oldy + k) * lineBytes + oldx * blockBytes],
						blockBytes))
					break;
			}
			if (k > bestmatches) {
				bestmatches = k;
				best = i;
			}
			if (k == maxLines)
				break;
		}

		//printf("Best had %lu matching lines of allowed %lu\n", bestmatches, maxLines);

		*outlines = bestmatches;
		*outx = (matches[best].idx & hashAnd) * SCROLLBLOCK_SIZE;
		*outy = matches[best].idx >> hashShift;

		// Was it a good match? If so, store for later
		if (*outx == inx && bestmatches >= maxLines / 2 &&
			totals[smallIdx] < 4 && *outy != iny) {
			lastOffX = 0;
			lastOffY = *outy - iny;
		}
	}

	void findBlock(const uint8_t * const ptr,
				const uint_fast32_t inx, const uint_fast32_t iny,
				uint_fast32_t *outx,
				uint_fast32_t *outy,
				uint_fast32_t *outlines) const {

		uint_fast32_t i, lowest = 0, tmpx = 0, tmpy = 0, tmplines,
				searchHash, lowestTotal = 10000, tmpTotal;

		*outlines = 0;

		for (i = 0; i < SCROLLBLOCK_SIZE; i++) {
			searchHash = (uint32_t) XXH64(ptr + lineBytes * i, blockBytes, 0);
			const uint_fast32_t smallIdx = searchHash % NUM_TOTALS;
			tmpTotal = totals[smallIdx];
			if (!tmpTotal)
				return;

			if (tmpTotal < lowestTotal) {
				lowest = i;
				lowestTotal = tmpTotal;
			}
		}

		// If the lowest number of matches is too high, we probably can't find
		// a full block
		if (lowestTotal > MAX_CHECKS)
			return;

		//printf("Lowest was %lu, %lu totals\n", lowest, lowestTotal);

		findBestMatch(ptr + lineBytes * lowest, SCROLLBLOCK_SIZE - lowest,
				inx, iny + lowest, &tmpx, &tmpy, &tmplines);

		// The end didn't match
		if (tmplines != SCROLLBLOCK_SIZE - lowest)
			return;

		if (tmpx != inx)
			return;

		// Source too high?
		if (tmpy < lowest)
			return;

		// Try to see if the beginning matches
		for (i = 0; i < lowest; i++) {
			if (!hashtable[((tmpy - lowest + i) << hashShift) + inx / SCROLLBLOCK_SIZE].hash)
				return; // Invalidated
			if (memcmp(ptr + lineBytes * i,
					&olddata[(tmpy - lowest + i) * lineBytes + tmpx * d],
					blockBytes))
				return;
		}

		*outlines = 64;
		*outx = tmpx;
		*outy = tmpy - lowest;
	}
};

#undef NUM_TOTALS
#define NUM_TOTALS (1024 * 1024 * 2)

class scrollHasher_bothDir_t: public scrollHasher_t {
public:
	scrollHasher_bothDir_t(): scrollHasher_t() {

		totals = (uint32_t *) malloc(sizeof(uint32_t) * NUM_TOTALS);
		starts = (uint32_t *) malloc(sizeof(uint32_t) * NUM_TOTALS);
		curs = (uint32_t *) malloc(sizeof(uint32_t) * NUM_TOTALS);
	}

	void calcHashes(const uint8_t *ptr,
			const uint32_t w_, const uint32_t h_, const uint32_t d_) {

		if (w != w_ || h != h_) {
			// Reallocate
			w = w_;
			h = h_;
			d = d_;
			lineBytes = w * d;
			blockBytes = SCROLLBLOCK_SIZE * d;

			hashw = npow(w - (SCROLLBLOCK_SIZE - 1));
			hashAnd = hashw - 1;
			hashShift = pow2shift(hashw);

			hashtable = (hashdata_t *) realloc(hashtable,
								hashw * h * sizeof(uint32_t));
			idxtable = (uint32_t *) realloc(idxtable,
								w * h * sizeof(uint32_t));

			olddata = (const uint8_t *) realloc((void *) olddata, w * h * d);
		}

		// We need to make a copy, since the comparer incrementally updates its copy
		memcpy((uint8_t *) olddata, ptr, w * h * d);

		Adler32 rolling(blockBytes);

		//memset(hashtable, 0, hashw * h * sizeof(uint32_t));
		//memset(idxtable, 0, w * h * sizeof(uint32_t));
		memset(totals, 0, NUM_TOTALS * sizeof(uint32_t));
		//memset(starts, 0, NUM_TOTALS * sizeof(uint32_t));
		//memset(curs, 0, NUM_TOTALS * sizeof(uint32_t));

		const uint8_t *prevptr = NULL;
		for (uint_fast32_t y = 0; y < h; y++) {
			const uint8_t *inptr0 = olddata;
			inptr0 += y * lineBytes;
			for (uint_fast32_t x = 0; x < w - (SCROLLBLOCK_SIZE - 1); x++) {
				if (!x) {
					rolling.reset();
					uint_fast32_t g;
					for (g = 0; g < SCROLLBLOCK_SIZE; g++) {
						for (uint_fast32_t di = 0; di < d; di++) {
							rolling.eat(inptr0[g * d + di]);
						}
					}
				} else {
					for (uint_fast32_t di = 0; di < d; di++) {
						rolling.update(prevptr[di],
								inptr0[(SCROLLBLOCK_SIZE - 1) * d + di]);
					}
				}
				const uint_fast32_t idx = (y << hashShift) + x;
				hashtable[idx].hash = rolling.hash;
				totals[rolling.hash % NUM_TOTALS]++;

				prevptr = inptr0;
				inptr0 += d;
			}
		}

		// calculate number of unique 21-bit hashes
		/*uint_fast32_t uniqHashes = 0;
		for (uint_fast32_t i = 0; i < NUM_TOTALS; i++) {
			if (totals[i])
				uniqHashes++;
		}
		printf("%lu unique hashes\n", uniqHashes);*/

		// Update starting positions
		uint_fast32_t sum = 0;
		for (uint_fast32_t i = 0; i < NUM_TOTALS; i++) {
			if (!totals[i])
				continue;
			starts[i] = curs[i] = sum;
			sum += totals[i];
		}

		// update index table
		const hashdata_t *src = hashtable;
		for (uint_fast32_t y = 0; y < h; y++) {
			uint_fast32_t ybase = (y << hashShift);
			for (uint_fast32_t x = 0; x < w - (SCROLLBLOCK_SIZE - 1); x++, ybase++) {
				const uint_fast32_t val = src[x].hash;
				const uint_fast32_t smallIdx = val % NUM_TOTALS;

				const uint_fast32_t newpos = curs[smallIdx]++;
				// this assert is very heavy, uncomment only for debugging
				//assert(curs[smallIdx] - starts[smallIdx] <= totals[smallIdx]);
				idxtable[newpos] = ybase;
			}
			src += hashw;
		}

		lastOffX = lastOffY = 0;
	}

	void invalidate(const uint_fast32_t x, uint_fast32_t y, uint_fast32_t h) {

		const uint_fast32_t nw = SCROLLBLOCK_SIZE;
		const uint_fast32_t left = x > (SCROLLBLOCK_SIZE - 1) ?
					(SCROLLBLOCK_SIZE - 1) : x;
		const uint_fast32_t right = x + nw + (SCROLLBLOCK_SIZE - 1) < w ?
					(SCROLLBLOCK_SIZE - 1) : w - x - nw;

		h += y;
		for (; y < h; y++) {
			memset(&hashtable[(y << hashShift) + x - left], 0,
				sizeof(uint32_t) * (nw + left + right));
		}
	}

	void findBestMatch(const uint8_t * const ptr, const uint_fast32_t maxLines,
				const uint_fast32_t inx, const uint_fast32_t iny,
				uint_fast32_t *outx,
				uint_fast32_t *outy,
				uint_fast32_t *outlines) const {
		const uint_fast32_t starthash = adler32(adler32(0, NULL, 0), ptr,
							blockBytes);
		const uint_fast32_t smallIdx = starthash % NUM_TOTALS;
		match_t matches[MAX_CHECKS];

		*outlines = 0;

		uint_fast32_t i, upto, curidx, curhash, found = 0, inc;
		upto = totals[smallIdx] + starts[smallIdx];
		if (!totals[smallIdx])
			return;

		inc = totals[smallIdx] / 32;
		if (!inc)
			inc = 1;

		//printf("target hash %lx, it has %u matches\n",
		//	starthash, totals[smallIdx]);

		// First, try the last good offset. If this was a scroll,
		// and we have a good offset, it should match almost everything
		const uint_fast16_t tryX = inx + lastOffX;
		const uint_fast16_t tryY = iny + lastOffY;
		if ((lastOffX || lastOffY) &&
			tryX < w - (SCROLLBLOCK_SIZE - 1) &&
			tryY < h - maxLines) {

			//printf("Trying good offset %ld,%ld for in %lu,%lu, try %lu,%lu\n",
			//	lastOffX, lastOffY, inx, iny, tryX, tryY);

			curidx = (tryY << hashShift) + tryX;
			curhash = hashtable[curidx].hash;
			if (curhash == starthash &&
				memcmp(ptr, &olddata[tryY * lineBytes + tryX * d], blockBytes) == 0) {

				matches[0].hash = curhash;
				matches[0].idx = curidx;
				found++;
			} /*else printf("Nope, hashes %u %lx %lx, mem %u, maxlines %lu\n",
				curhash == starthash, curhash, starthash,
				memcmp(ptr, &olddata[tryY * lineBytes + tryX * d], blockBytes) == 0,
				maxLines);*/
		}

		for (i = starts[smallIdx]; i < upto; i += inc) {
			curidx = idxtable[i];
			curhash = hashtable[curidx].hash;

			if (curhash != starthash)
				continue;

			// Convert to olddata position
			const uint_fast32_t oldy = curidx >> hashShift;
			const uint_fast32_t oldx = curidx & hashAnd;

			if (memcmp(ptr, &olddata[oldy * lineBytes + oldx * d], blockBytes))
				continue;

			matches[found].hash = curhash;
			matches[found].idx = curidx;
			found++;
			if (found >= MAX_CHECKS)
				break;
		}

		if (!found)
			return;

		//printf("%lu of those were suitable for further checks\n", found);

		// Find best of them
		uint_fast32_t best = 0, bestmatches = 0;
		for (i = 0; i < found; i++) {

			const uint_fast32_t oldy = matches[i].idx >> hashShift;
			const uint_fast32_t oldx = matches[i].idx & hashAnd;

			uint_fast32_t k, bothMaxLines;
			bothMaxLines = maxLines;
			if (bothMaxLines > h - oldy)
				bothMaxLines = h - oldy;
			for (k = 1; k < bothMaxLines; k++) {
/*				curhash = adler32(adler32(0, NULL, 0), ptr + lineBytes * k,
							blockBytes);
				if (curhash != hashtable[matches[i].idx + hashw * k].hash)
					break;*/
				if (!hashtable[matches[i].idx + (k << hashShift)].hash)
					break; // Invalidated
				if (memcmp(ptr + lineBytes * k,
						&olddata[(oldy + k) * lineBytes + oldx * d],
						blockBytes))
					break;
			}
			if (k > bestmatches) {
				bestmatches = k;
				best = i;
			}
			if (k == maxLines)
				break;
		}

		//printf("Best had %lu matching lines of allowed %lu\n", bestmatches, maxLines);

		*outlines = bestmatches;
		*outx = matches[best].idx & hashAnd;
		*outy = matches[best].idx >> hashShift;

		// Was it a good match? If so, store for later
		if (bestmatches >= maxLines / 2 &&
			totals[smallIdx] < 4) {
			lastOffX = *outx - inx;
			lastOffY = *outy - iny;
		}
	}

	void findBlock(const uint8_t * const ptr,
				const uint_fast32_t inx, const uint_fast32_t iny,
				uint_fast32_t *outx,
				uint_fast32_t *outy,
				uint_fast32_t *outlines) const {
		*outlines = 0;
		// Not implemented for horizontal
	}
};

ComparingUpdateTracker::ComparingUpdateTracker(PixelBuffer* buffer)
  : fb(buffer), oldFb(fb->getPF(), 0, 0), firstCompare(true),
    enabled(true), detectScroll(false), totalPixels(0), missedPixels(0),
    scrollHasher(NULL)
{
    changed.assign_union(fb->getRect());
    if (Server::detectHorizontal)
      scrollHasher = new scrollHasher_bothDir_t;
    else
      scrollHasher = new scrollHasher_vert_t;
}

ComparingUpdateTracker::~ComparingUpdateTracker()
{
    delete scrollHasher;
}


#define BLOCK_SIZE 64

bool ComparingUpdateTracker::compare(bool skipScrollDetection, const Region &skipCursorArea)
{
  std::vector<Rect> rects;
  std::vector<Rect>::iterator i;

  changedPerc = 100;

  if (!enabled)
    return false;

  if (firstCompare) {
    // NB: We leave the change region untouched on this iteration,
    // since in effect the entire framebuffer has changed.
    oldFb.setSize(fb->width(), fb->height());

    for (int y=0; y<fb->height(); y+=BLOCK_SIZE) {
      Rect pos(0, y, fb->width(), __rfbmin(fb->height(), y+BLOCK_SIZE));
      int srcStride;
      const rdr::U8* srcData = fb->getBuffer(pos, &srcStride);
      oldFb.imageRect(pos, srcData, srcStride);
    }

    firstCompare = false;

    return false;
  }

  copied.get_rects(&rects, copy_delta.x<=0, copy_delta.y<=0);
  for (i = rects.begin(); i != rects.end(); i++)
    oldFb.copyRect(*i, copy_delta);

  changed.get_rects(&rects);

  uint32_t changedArea = 0;
  bool atLeast64 = false;
  detectScroll = false;
  for (i = rects.begin(); i != rects.end(); i++) {
    if (!atLeast64 && i->width() >= 64)
      atLeast64 = true;
    changedArea += i->area();
  }
  if (atLeast64 && Server::detectScrolling && !skipScrollDetection &&
      (changedArea * 100) / (fb->width() * fb->height()) > (unsigned) Server::scrollDetectLimit) {
    detectScroll = true;
    Rect pos(0, 0, oldFb.width(), oldFb.height());
    int unused;
    scrollHasher->calcHashes(oldFb.getBuffer(pos, &unused), oldFb.width(), oldFb.height(),
    				oldFb.getPF().bpp / 8);
    // Invalidating lossy areas is not needed, the lossy region tracking tracks copies too
  }

  copyPassRects.clear();

  Region newChanged;
  for (i = rects.begin(); i != rects.end(); i++)
    compareRect(*i, &newChanged, skipCursorArea);

  changed.get_rects(&rects);
  for (i = rects.begin(); i != rects.end(); i++)
    totalPixels += i->area();
  newChanged.get_rects(&rects);
  unsigned newchangedarea = 0;
  for (i = rects.begin(); i != rects.end(); i++) {
    missedPixels += i->area();
    newchangedarea += i->area();
  }

  changedPerc = newchangedarea * 100 / fb->area();

  if (changed.equals(newChanged))
    return false;

  changed = newChanged;

  return true;
}

void ComparingUpdateTracker::enable()
{
  enabled = true;
}

void ComparingUpdateTracker::disable()
{
  enabled = false;

  // Make sure we update the framebuffer next time we get enabled
  firstCompare = true;
}

static void tryMerge(std::vector<CopyPassRect> &copyPassRects,
                     const int y, const int blockLeft,
                     const int blockRight, const uint_fast32_t outlines,
                     const uint_fast32_t outx, const uint_fast32_t outy) {

  if (copyPassRects.size() &&
    copyPassRects.back().rect.tl.y == y &&
    copyPassRects.back().rect.br.x == blockLeft &&
    (unsigned) copyPassRects.back().rect.br.y == y + outlines &&
    copyPassRects.back().src_x + copyPassRects.back().rect.width() == outx &&
    copyPassRects.back().src_y == outy) {

    CopyPassRect &prev = copyPassRects.back();
    prev.rect.br.x += SCROLLBLOCK_SIZE;

    //merged++;
  } else {
    // Before adding this new rect as a non-mergeable one, try to vertically merge the
    // previous two
    if (copyPassRects.size() > 1) {
      CopyPassRect &prev = *(copyPassRects.end() - 2);
      const CopyPassRect &cur = copyPassRects.back();

      if (prev.rect.br.y == cur.rect.tl.y &&
          prev.rect.tl.x == cur.rect.tl.x &&
          prev.rect.br.x == cur.rect.br.x &&
          prev.src_x == cur.src_x &&
          prev.src_y + prev.rect.height() == cur.src_y) {

        prev.rect.br.y += cur.rect.height();
        copyPassRects.pop_back();
      }
    }

    const CopyPassRect cp = {Rect(blockLeft, y, blockRight, y + outlines),
                            (unsigned) outx, (unsigned) outy};
    copyPassRects.push_back(cp);
 }
}

void ComparingUpdateTracker::compareRect(const Rect& inr, Region* newChanged,
                                         const Region &skipCursorArea)
{
    Rect r = inr;
    if (detectScroll && !Server::detectHorizontal)
      r.tl.x &= ~(BLOCK_SIZE - 1);

  if (!r.enclosed_by(fb->getRect())) {
    Rect safe;
    // Crop the rect and try again
    safe = r.intersect(fb->getRect());
    if (!safe.is_empty())
      compareRect(safe, newChanged, skipCursorArea);
    return;
  }

  int bytesPerPixel = fb->getPF().bpp/8;
  int oldStride;
  rdr::U8* oldData = oldFb.getBufferRW(r, &oldStride);
  int oldStrideBytes = oldStride * bytesPerPixel;

  std::vector<Rect> changedBlocks;

  for (int blockTop = r.tl.y; blockTop < r.br.y; blockTop += BLOCK_SIZE)
  {
    // Get a strip of the source buffer
    Rect pos(r.tl.x, blockTop, r.br.x, __rfbmin(r.br.y, blockTop+BLOCK_SIZE));
    int fbStride;
    const rdr::U8* newBlockPtr = fb->getBuffer(pos, &fbStride);
    int newStrideBytes = fbStride * bytesPerPixel;

    rdr::U8* oldBlockPtr = oldData;
    int blockBottom = __rfbmin(blockTop+BLOCK_SIZE, r.br.y);

    for (int blockLeft = r.tl.x; blockLeft < r.br.x; blockLeft += BLOCK_SIZE)
    {
      const rdr::U8* newPtr = newBlockPtr;
      rdr::U8* oldPtr = oldBlockPtr;

      int blockRight = __rfbmin(blockLeft+BLOCK_SIZE, r.br.x);
      int blockWidthInBytes = (blockRight-blockLeft) * bytesPerPixel;
      bool changed = false;
      int y;

      for (y = blockTop; y < blockBottom; y++)
      {
        if (memcmp(oldPtr, newPtr, blockWidthInBytes) != 0)
        {
          // A block has changed - copy the remainder to the oldFb
          changed = true;
          const rdr::U8* savedPtr = newPtr;
          for (int y2 = y; y2 < blockBottom; y2++)
          {
            memcpy(oldPtr, newPtr, blockWidthInBytes);
            newPtr += newStrideBytes;
            oldPtr += oldStrideBytes;
          }
          newPtr = savedPtr;
          break;
        }

        newPtr += newStrideBytes;
        oldPtr += oldStrideBytes;
      }

      if (!changed || (changed && !detectScroll) ||
          (skipCursorArea.numRects() &&
           !skipCursorArea.intersect(Rect(blockLeft, blockTop, blockRight, blockBottom)).is_empty())) {
        if (changed || skipCursorArea.numRects())
          changedBlocks.push_back(Rect(blockLeft, blockTop,
                                       blockRight, blockBottom));

        oldBlockPtr += blockWidthInBytes;
        newBlockPtr += blockWidthInBytes;
        continue;
      }

      uint_fast32_t outx, outy, outlines;
      if (blockRight - blockLeft < SCROLLBLOCK_SIZE) {
        // Block too small, put it out outright as changed
        changedBlocks.push_back(Rect(blockLeft, blockTop,
                                     blockRight, blockBottom));
      } else {
        // First, try to find a full block
        outlines = 0;
        if (blockBottom - blockTop == SCROLLBLOCK_SIZE)
          scrollHasher->findBlock(newBlockPtr, blockLeft, blockTop, &outx, &outy,
                                 &outlines);

        if (outlines == SCROLLBLOCK_SIZE) {
          // Perfect match!
          // success += outlines;
          tryMerge(copyPassRects, blockTop, blockLeft, blockRight, outlines, outx, outy);

          scrollHasher->invalidate(blockLeft, blockTop, outlines);

          oldBlockPtr += blockWidthInBytes;
          newBlockPtr += blockWidthInBytes;
          continue;
        }

        for (; y < blockBottom; y += outlines)
        {
          // We have the first changed line. Find the best match, if any
          scrollHasher->findBestMatch(newPtr, blockBottom - y, blockLeft, y,
                                      &outx, &outy, &outlines);

          if (!outlines) {
            // Heuristic, if a line did not match, probably
            // the next few won't either
            changedBlocks.push_back(Rect(blockLeft, y,
                                         blockRight, __rfbmin(y + 4, blockBottom)));
            y += 4;
            newPtr += newStrideBytes * 4;
            // unfound += 4;
            continue;
          }
          // success += outlines;

          // Try to merge it with the last rect
          tryMerge(copyPassRects, y, blockLeft, blockRight, outlines, outx, outy);

          scrollHasher->invalidate(blockLeft, y, outlines);

          newPtr += newStrideBytes * outlines;
        }
      }

      oldBlockPtr += blockWidthInBytes;
      newBlockPtr += blockWidthInBytes;
    }

    oldData += oldStrideBytes * BLOCK_SIZE;
  }

  oldFb.commitBufferRW(r);

  if (!changedBlocks.empty()) {
    Region temp;
    temp.setOrderedRects(changedBlocks);
    newChanged->assign_union(temp);
  }
}

void ComparingUpdateTracker::logStats()
{
  double ratio;
  char a[1024], b[1024];

  siPrefix(totalPixels, "pixels", a, sizeof(a));
  siPrefix(missedPixels, "pixels", b, sizeof(b));

  ratio = (double)totalPixels / missedPixels;

  vlog.info("%s in / %s out", a, b);
  vlog.info("(1:%g ratio)", ratio);

  totalPixels = missedPixels = 0;
}

void ComparingUpdateTracker::getUpdateInfo(UpdateInfo* info, const Region& cliprgn)
{
  info->copypassed = copyPassRects;
  SimpleUpdateTracker::getUpdateInfo(info, cliprgn);
}

void ComparingUpdateTracker::clear()
{
  copyPassRects.clear();
  SimpleUpdateTracker::clear();
}

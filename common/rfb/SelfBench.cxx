/* Copyright (C) 2021 Kasm Web
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

#include <rfb/ComparingUpdateTracker.h>
#include <rfb/EncodeManager.h>
#include <rfb/LogWriter.h>
#include <rfb/SConnection.h>
#include <rfb/ServerCore.h>
#include <rfb/PixelBuffer.h>
#include <rfb/TightJPEGEncoder.h>
#include <rfb/TightWEBPEncoder.h>
#include <rfb/util.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdlib.h>

using namespace rfb;
static LogWriter vlog("SelfBench");

static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);

#define RUNS 64

#define W 1600
#define H 1200

void SelfBench() {

	unsigned i, runs;
	struct timeval start;

	ManagedPixelBuffer f1(pfRGBX, W, H);
	ManagedPixelBuffer f2(pfRGBX, W, H);
	ManagedPixelBuffer screen(pfRGBX, W, H);

	int stride;
	rdr::U8 *f1ptr = f1.getBufferRW(f1.getRect(), &stride);
	rdr::U8 *f2ptr = f2.getBufferRW(f2.getRect(), &stride);
	rdr::U8 * const screenptr = screen.getBufferRW(screen.getRect(), &stride);

	rdr::U8 * const f1orig = f1ptr;
	rdr::U8 * const f2orig = f2ptr;

	for (i = 0; i < W * H * 4; i += 4) {
		f1ptr[0] = rand();
		f1ptr[1] = rand();
		f1ptr[2] = rand();

		f2ptr[0] = rand();
		f2ptr[1] = rand();
		f2ptr[2] = rand();

		f1ptr += 4;
		f2ptr += 4;
	}

	vlog.info("Running micro-benchmarks (single-threaded, runs depending on task)");

	// Encoding
	std::vector<uint8_t> vec;

	TightJPEGEncoder jpeg(NULL);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		jpeg.compressOnly(&f1, 8, vec, false);
	}
	vlog.info("Jpeg compression at quality 8 took %u ms (%u runs)", msSince(&start), runs);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		jpeg.compressOnly(&f1, 4, vec, false);
	}
	vlog.info("Jpeg compression at quality 4 took %u ms (%u runs)", msSince(&start), runs);


	TightWEBPEncoder webp(NULL);

	gettimeofday(&start, NULL);
	runs = RUNS / 8;
	for (i = 0; i < runs; i++) {
		webp.compressOnly(&f1, 8, vec, false);
	}
	vlog.info("Webp compression at quality 8 took %u ms (%u runs)", msSince(&start), runs);

	gettimeofday(&start, NULL);
	runs = RUNS / 4;
	for (i = 0; i < runs; i++) {
		webp.compressOnly(&f1, 4, vec, false);
	}
	vlog.info("Webp compression at quality 4 took %u ms (%u runs)", msSince(&start), runs);

	// Scaling
	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		PixelBuffer *pb = nearestScale(&f1, W * 0.8, H * 0.8, 0.8);
		delete pb;
	}
	vlog.info("Nearest scaling to 80%% took %u ms (%u runs)", msSince(&start), runs);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		PixelBuffer *pb = nearestScale(&f1, W * 0.4, H * 0.4, 0.4);
		delete pb;
	}
	vlog.info("Nearest scaling to 40%% took %u ms (%u runs)", msSince(&start), runs);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		PixelBuffer *pb = bilinearScale(&f1, W * 0.8, H * 0.8, 0.8);
		delete pb;
	}
	vlog.info("Bilinear scaling to 80%% took %u ms (%u runs)", msSince(&start), runs);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		PixelBuffer *pb = bilinearScale(&f1, W * 0.4, H * 0.4, 0.4);
		delete pb;
	}
	vlog.info("Bilinear scaling to 40%% took %u ms (%u runs)", msSince(&start), runs);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		PixelBuffer *pb = progressiveBilinearScale(&f1, W * 0.8, H * 0.8, 0.8);
		delete pb;
	}
	vlog.info("Progressive bilinear scaling to 80%% took %u ms (%u runs)", msSince(&start), runs);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		PixelBuffer *pb = progressiveBilinearScale(&f1, W * 0.4, H * 0.4, 0.4);
		delete pb;
	}
	vlog.info("Progressive bilinear scaling to 40%% took %u ms (%u runs)", msSince(&start), runs);

	// Analysis
	ComparingUpdateTracker *comparer = new ComparingUpdateTracker(&screen);
	Region cursorReg;

	Server::detectScrolling.setParam(false);
	Server::detectHorizontal.setParam(false);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		memcpy(screenptr, i % 2 ? f1orig : f2orig, W * H * 4);
		comparer->compare(true, cursorReg);
	}
	vlog.info("Analysis took %u ms (%u runs) (incl. memcpy overhead)", msSince(&start), runs);

	Server::detectScrolling.setParam(true);

	gettimeofday(&start, NULL);
	runs = RUNS;
	for (i = 0; i < runs; i++) {
		memcpy(screenptr, i % 2 ? f1orig : f2orig, W * H * 4);
		comparer->compare(false, cursorReg);
	}
	vlog.info("Analysis w/ scroll detection took %u ms (%u runs) (incl. memcpy overhead)", msSince(&start), runs);

	Server::detectHorizontal.setParam(true);
	delete comparer;
	comparer = new ComparingUpdateTracker(&screen);

	gettimeofday(&start, NULL);
	runs = RUNS / 2;
	for (i = 0; i < runs; i++) {
		memcpy(screenptr, i % 2 ? f1orig : f2orig, W * H * 4);
		comparer->compare(false, cursorReg);
	}
	vlog.info("Analysis w/ horizontal scroll detection took %u ms (%u runs) (incl. memcpy overhead)", msSince(&start), runs);

	exit(0);
}

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
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <tinyxml2.h>

using namespace rfb;
static LogWriter vlog("SelfBench");

static const PixelFormat pfRGBX(32, 24, false, true, 255, 255, 255, 0, 8, 16);

static constexpr uint32_t RUNS = 64;

static constexpr uint32_t WIDTH = 1600;
static constexpr uint32_t HEIGHT = 1200;

void SelfBench() {
	tinyxml2::XMLDocument doc;

	auto *test_suit = doc.NewElement("testsuite");
	test_suit->SetAttribute("name", "SelfBench");

	doc.InsertFirstChild(test_suit);

	ManagedPixelBuffer f1(pfRGBX, WIDTH, HEIGHT);
	ManagedPixelBuffer f2(pfRGBX, WIDTH, HEIGHT);
	ManagedPixelBuffer screen(pfRGBX, WIDTH, HEIGHT);

	int stride;
	rdr::U8 *f1ptr = f1.getBufferRW(f1.getRect(), &stride);
	rdr::U8 *f2ptr = f2.getBufferRW(f2.getRect(), &stride);
	rdr::U8 * const screenptr = screen.getBufferRW(screen.getRect(), &stride);

	rdr::U8 * const f1orig = f1ptr;
	rdr::U8 * const f2orig = f2ptr;

	for (uint32_t i = 0; i < WIDTH * HEIGHT * 4; i += 4) {
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

	TightJPEGEncoder jpeg(nullptr);

	uint32_t test_cases {};
	uint64_t total_time {};

	auto benchmark = [&doc, &test_suit, &test_cases, &total_time](const char *name, uint32_t runs, auto func) {
		auto now = std::chrono::high_resolution_clock::now();
		for (uint32_t i = 0; i < runs; i++) {
			func(i);
		}

		++test_cases;
		auto value = elapsedMs(now);
		double junit_value = value / 1000.;
		total_time += value;

		vlog.info("%s took %lu ms (%u runs)", name, value, runs);
		auto *test_case = doc.NewElement("testcase");
		test_case->SetAttribute("name", name);
		test_case->SetAttribute("time", junit_value);
		test_case->SetAttribute("runs", runs);
		test_case->SetAttribute("classname", "KasmVNC");
		test_suit->InsertEndChild(test_case);
	};

	benchmark("Jpeg compression at quality 8", RUNS, [&jpeg, &vec, &f1](uint32_t) {
		jpeg.compressOnly(&f1, 8, vec, false);
	});

	benchmark("Jpeg compression at quality 4", RUNS, [&jpeg, &vec, &f1](uint32_t) {
		jpeg.compressOnly(&f1, 4, vec, false);
	});

	TightWEBPEncoder webp(nullptr);

	benchmark("Webp compression at quality 8", RUNS / 8, [&webp,&f1, &vec](uint32_t) {
		webp.compressOnly(&f1, 8, vec, false);
	});

	benchmark("Webp compression at quality 4", RUNS / 4, [&webp, &f1, &vec](uint32_t) {
		webp.compressOnly(&f1, 4, vec, false);
	});

	// Scaling
	benchmark("Nearest scaling to 80%", RUNS, [&f1](uint32_t) {
		PixelBuffer *pb = nearestScale(&f1, WIDTH * 0.8, HEIGHT * 0.8, 0.8);
		delete pb;
	});

	benchmark("Nearest scaling to 40%", RUNS, [&f1](uint32_t) {
		PixelBuffer *pb = nearestScale(&f1, WIDTH * 0.4, HEIGHT * 0.4, 0.4);
		delete pb;
	});

	benchmark("Bilinear scaling to 80%", RUNS, [&f1](uint32_t) {
		PixelBuffer *pb = bilinearScale(&f1, WIDTH * 0.8, HEIGHT * 0.8, 0.8);
		delete pb;
	});

	benchmark("Bilinear scaling to 40%", RUNS, [&f1](uint32_t) {
		PixelBuffer *pb = bilinearScale(&f1, WIDTH * 0.4, HEIGHT * 0.4, 0.4);
		delete pb;
	});


	benchmark("Progressive bilinear scaling to 80%", RUNS, [&f1](uint32_t) {
		PixelBuffer *pb = progressiveBilinearScale(&f1, WIDTH * 0.8, HEIGHT * 0.8, 0.8);
		delete pb;
	});
	benchmark("Progressive bilinear scaling to 40%", RUNS, [&f1](uint32_t) {
		PixelBuffer *pb = progressiveBilinearScale(&f1, WIDTH * 0.4, HEIGHT * 0.4, 0.4);
		delete pb;
	});

	// Analysis
	auto *comparer = new ComparingUpdateTracker(&screen);
	Region cursorReg;

	Server::detectScrolling.setParam(false);
	Server::detectHorizontal.setParam(false);

	benchmark("Analysis (incl. memcpy overhead)", RUNS,
	          [&screenptr, &comparer, &cursorReg, f1orig, f2orig](uint32_t i) {
		          memcpy(screenptr, i % 2 ? f1orig : f2orig, WIDTH * HEIGHT * 4);
		          comparer->compare(true, cursorReg);
	          });

	Server::detectScrolling.setParam(true);

	benchmark("Analysis w/ scroll detection (incl. memcpy overhead)", RUNS,
	          [&screenptr, &comparer, &cursorReg, f1orig, f2orig](uint32_t i) {
		          memcpy(screenptr, i % 2 ? f1orig : f2orig, WIDTH * HEIGHT * 4);
		          comparer->compare(false, cursorReg);
	          });

	Server::detectHorizontal.setParam(true);
	delete comparer;
	comparer = new ComparingUpdateTracker(&screen);

	benchmark("Analysis w/ horizontal scroll detection (incl. memcpy overhead)", RUNS / 2,
	          [&screenptr, &comparer, &cursorReg, f1orig, f2orig](uint32_t i) {
		          memcpy(screenptr, i % 2 ? f1orig : f2orig, WIDTH * HEIGHT * 4);
		          comparer->compare(false, cursorReg);
	          });

	test_suit->SetAttribute("tests", test_cases);
	test_suit->SetAttribute("failures", 0);
	test_suit->SetAttribute("time", total_time);

	doc.SaveFile("SelfBench.xml");

	exit(0);
}

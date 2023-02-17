/* Copyright (C) 2023 Kasm
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

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <rfb/LogWriter.h>
#include <rfb/ServerCore.h>
#include <rfb/VNCServerST.h>

#include "Watermark.h"

using namespace rfb;

static LogWriter vlog("watermark");

watermarkInfo_t watermarkInfo;

uint8_t *watermarkData, *watermarkUnpacked, *watermarkTmp;
uint32_t watermarkDataLen;
static uint16_t rw, rh;

#define MAXW 4096
#define MAXH 4096

static bool loadimage(const char path[]) {

	FILE *f = fopen(path, "r");
	if (!f) {
		vlog.error("Can't open %s", path);
		return false;
	}

	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL);
	if (!png_ptr) return false;
	png_infop info = png_create_info_struct(png_ptr);
	if (!info) return false;
	if (setjmp(png_jmpbuf(png_ptr))) return false;

	png_init_io(png_ptr, f);
	png_read_png(png_ptr, info,
		PNG_TRANSFORM_PACKING |
		PNG_TRANSFORM_STRIP_16 |
		PNG_TRANSFORM_STRIP_ALPHA |
		PNG_TRANSFORM_EXPAND, NULL);

	uint8_t **rows = png_get_rows(png_ptr, info);
	const unsigned imgw = png_get_image_width(png_ptr, info);
	const unsigned imgh = png_get_image_height(png_ptr, info);

	watermarkInfo.w = imgw;
	watermarkInfo.h = imgh;
	watermarkInfo.src = (uint8_t *) calloc(imgw, imgh);

	unsigned x, y;
	for (y = 0; y < imgh; y++) {
		for (x = 0; x < imgw; x++) {
			const uint8_t r = rows[y][x * 3 + 0];
			const uint8_t g = rows[y][x * 3 + 1];
			const uint8_t b = rows[y][x * 3 + 2];

			const uint8_t grey = r * .2126f +
						g * .7152f +
						b * .0722f;

			const uint8_t out = (grey + 8) >> 4;
			watermarkInfo.src[y * imgw + x] = out < 16 ? out : 15;
		}
	}

	fclose(f);
	png_destroy_info_struct(png_ptr, &info);
	png_destroy_read_struct(&png_ptr, NULL, NULL);

	return true;
}

bool watermarkInit() {
	memset(&watermarkInfo, 0, sizeof(watermarkInfo_t));
	watermarkData = watermarkUnpacked = watermarkTmp = NULL;
	rw = rh = 0;

	if (!Server::DLP_WatermarkImage[0])
		return true;

	if (!loadimage(Server::DLP_WatermarkImage))
		return false;

	if (Server::DLP_WatermarkRepeatSpace && Server::DLP_WatermarkLocation[0]) {
		vlog.error("Repeat and location can't be used together");
		return false;
	}

	if (sscanf(Server::DLP_WatermarkTint, "%hhu,%hhu,%hhu,%hhu",
			&watermarkInfo.r,
			&watermarkInfo.g,
			&watermarkInfo.b,
			&watermarkInfo.a) != 4) {
		vlog.error("Invalid tint");
		return false;
	}

	watermarkInfo.repeat = Server::DLP_WatermarkRepeatSpace;

	if (Server::DLP_WatermarkLocation[0]) {
		if (sscanf(Server::DLP_WatermarkLocation, "%hd,%hd",
			&watermarkInfo.x,
			&watermarkInfo.y) != 2) {
			vlog.error("Invalid location");
			return false;
		}
	}

	watermarkUnpacked = (uint8_t *) calloc(MAXW, MAXH);
	watermarkTmp = (uint8_t *) calloc(MAXW, MAXH / 2);
	watermarkData = (uint8_t *) calloc(MAXW, MAXH / 2);

	return true;
}

// update the screen-size rendered watermark whenever the screen is resized
void VNCServerST::updateWatermark() {
	if (rw == pb->width() &&
		rh == pb->height())
		return;

	rw = pb->width();
	rh = pb->height();

	memset(watermarkUnpacked, 0, rw * rh);

	uint16_t x, y, srcy;

	if (watermarkInfo.repeat) {
		for (y = 0, srcy = 0; y < rh; y++) {
			for (x = 0; x < rw;) {
				if (x + watermarkInfo.w < rw)
					memcpy(&watermarkUnpacked[y * rw + x],
						&watermarkInfo.src[srcy * watermarkInfo.w],
						watermarkInfo.w);
				else
					memcpy(&watermarkUnpacked[y * rw + x],
						&watermarkInfo.src[srcy * watermarkInfo.w],
						rw - x);

				x += watermarkInfo.w + watermarkInfo.repeat;
			}

			srcy++;
			if (srcy == watermarkInfo.h) {
				srcy = 0;
				y += watermarkInfo.repeat;
			}
		}
	} else {
		int16_t sx, sy;

		if (!watermarkInfo.x)
			sx = (rw - watermarkInfo.w) / 2;
		else if (watermarkInfo.x > 0)
			sx = watermarkInfo.x;
		else
			sx = rw - watermarkInfo.w + watermarkInfo.x;

		if (sx < 0)
			sx = 0;

		if (!watermarkInfo.y)
			sy = (rh - watermarkInfo.h) / 2;
		else if (watermarkInfo.y > 0)
			sy = watermarkInfo.y;
		else
			sy = rh - watermarkInfo.h + watermarkInfo.y;

		if (sy < 0)
			sy = 0;

		for (y = 0; y < watermarkInfo.h; y++) {
			if (sx + watermarkInfo.w < rw)
				memcpy(&watermarkUnpacked[(sy + y) * rw + sx],
					&watermarkInfo.src[y * watermarkInfo.w],
					watermarkInfo.w);
			else
				memcpy(&watermarkUnpacked[(sy + y) * rw + sx],
					&watermarkInfo.src[y * watermarkInfo.w],
					rw - sx);
		}
	}
}

void packWatermark(const Region &changed) {
	// Take the expanded 4-bit data, filter it by the changed rects, pack
	// to shared bytes, and compress with zlib

	uint16_t x, y;
	uint8_t pix[2], cur = 0;
	uint8_t *dst = watermarkTmp;

        const Rect &bounding = changed.get_bounding_rect();

	for (y = 0; y < rh; y++) {
		// Is the entire line outside the changed area?
		if (bounding.tl.y > y || bounding.br.y < y) {
			for (x = 0; x < rw; x++) {
				pix[cur] = 0;

				if (cur || (y == rh - 1 && x == rw - 1))
					*dst++ = pix[0] | (pix[1] << 4);

				cur ^= 1;
			}
		} else {
			for (x = 0; x < rw; x++) {
				pix[cur] = 0;
				if (bounding.contains(Point(x, y)) && changed.contains(x, y))
					pix[cur] = watermarkUnpacked[y * rw + x];

				if (cur || (y == rh - 1 && x == rw - 1))
					*dst++ = pix[0] | (pix[1] << 4);

				cur ^= 1;
			}
		}
	}

	uLong destLen = MAXW * MAXH / 2;
	if (compress2(watermarkData, &destLen, watermarkTmp, rw * rh / 2 + 1, 1) != Z_OK)
		vlog.error("Zlib compression error");

	watermarkDataLen = destLen;
}

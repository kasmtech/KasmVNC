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

#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include <rfb/LogWriter.h>
#include <rfb/ServerCore.h>
#include <rfb/VNCServerST.h>
#include "font.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include "Watermark.h"

using namespace rfb;

static LogWriter vlog("watermark");

watermarkInfo_t watermarkInfo;

uint8_t *watermarkData, *watermarkUnpacked, *watermarkTmp;
uint32_t watermarkDataLen;
static uint16_t rw, rh;
static time_t lastUpdate;

static FT_Library ft = NULL;
static FT_Face face;

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

// Note: w and h are absolute
static void str(uint8_t *buf, const char *txt, const uint32_t x_, const uint32_t y_,
		const uint32_t w, const uint32_t h,
		const uint32_t stride) {

	unsigned ucs[256], i, ucslen;
	unsigned len = strlen(txt);
	i = 0;
	ucslen = 0;
	while (len > 0 && txt[i]) {
		size_t ret = rfb::utf8ToUCS4(&txt[i], len, &ucs[ucslen]);
		i += ret;
		len -= ret;
		ucslen++;
	}

	uint32_t x, y;

	x = x_;
	y = y_;
	for (i = 0; i < ucslen; i++) {
		if (FT_Load_Char(face, ucs[i], FT_LOAD_RENDER))
			continue;
		const FT_Bitmap * const map = &(face->glyph->bitmap);

		if (FT_HAS_KERNING(face) && i) {
			FT_Vector delta;
			FT_Get_Kerning(face, ucs[i - 1], ucs[i], ft_kerning_default, &delta);
			x += delta.x >> 6;
		}

		uint32_t row, col;
		for (row = 0; row < (uint32_t) map->rows; row++) {
			int ny = row + y - face->glyph->bitmap_top;
			if (ny < 0)
				continue;
			if ((unsigned) ny >= h)
				continue;

			uint8_t *dst = (uint8_t *) buf;
			dst += ny * stride + x;

			const uint8_t *src = map->buffer + map->pitch * row;
			for (col = 0; col < (uint32_t) map->width; col++) {
				if (col + x >= w)
					continue;
				const uint8_t out = (src[col] + 8) >> 4;
				dst[col] = out < 16 ? out : 15;
			}
		}

		x += face->glyph->advance.x >> 6;
	}
}

static uint32_t drawnwidth(const char *txt) {

	unsigned ucs[256], i, ucslen;
	unsigned len = strlen(txt);
	i = 0;
	ucslen = 0;
	while (len > 0 && txt[i]) {
		size_t ret = rfb::utf8ToUCS4(&txt[i], len, &ucs[ucslen]);
		i += ret;
		len -= ret;
		ucslen++;
	}

	uint32_t x;

	x = 0;
	for (i = 0; i < ucslen; i++) {
		if (FT_Load_Char(face, ucs[i], FT_LOAD_DEFAULT))
			continue;

		if (FT_HAS_KERNING(face) && i) {
			FT_Vector delta;
			FT_Get_Kerning(face, ucs[i - 1], ucs[i], ft_kerning_default, &delta);
			x += delta.x >> 6;
		}

		x += face->glyph->advance.x >> 6;
	}

	return x;
}

static void angle2mat(FT_Matrix &mat) {
	const float angle = Server::DLP_WatermarkTextAngle / 360.f * 2 * -3.14159f;

	mat.xx = (FT_Fixed)( cosf(angle) * 0x10000L);
	mat.xy = (FT_Fixed)(-sinf(angle) * 0x10000L);
	mat.yx = (FT_Fixed)( sinf(angle) * 0x10000L);
	mat.yy = (FT_Fixed)( cosf(angle) * 0x10000L);
}

// Note: w and h are absolute
static void angledstr(uint8_t *buf, const char *txt, const uint32_t x_, const uint32_t y_,
		const uint32_t w, const uint32_t h,
		const uint32_t stride, const bool invx, const bool invy) {

	unsigned ucs[256], i, ucslen;
	unsigned len = strlen(txt);
	i = 0;
	ucslen = 0;
	while (len > 0 && txt[i]) {
		size_t ret = rfb::utf8ToUCS4(&txt[i], len, &ucs[ucslen]);
		i += ret;
		len -= ret;
		ucslen++;
	}

	FT_Matrix mat;
	FT_Vector pen;

	angle2mat(mat);

	pen.x = 0;
	pen.y = 0;

	uint32_t x, y;

	x = x_;
	y = y_;
	for (i = 0; i < ucslen; i++) {
		FT_Set_Transform(face, &mat, &pen);

		if (FT_Load_Char(face, ucs[i], FT_LOAD_RENDER))
			continue;
		const FT_Bitmap * const map = &(face->glyph->bitmap);

		uint32_t row, col;
		for (row = 0; row < (uint32_t) map->rows; row++) {
			int ny = row + y - face->glyph->bitmap_top;
			if (ny < 0)
				continue;
			if ((unsigned) ny >= h)
				continue;

			uint8_t *dst = (uint8_t *) buf;
			dst += ny * stride + x;

			const uint8_t *src = map->buffer + map->pitch * row;
			for (col = 0; col < (uint32_t) map->width; col++) {
				if (col + x >= w)
					continue;
				const uint8_t out = (src[col] + 8) >> 4;
				dst[col] |= out < 16 ? out : 15;
			}
		}

		x += face->glyph->advance.x >> 6;

		pen.x += face->glyph->advance.x;
		pen.y += face->glyph->advance.y;
	}
}

static void angledsize(const char *txt, uint32_t &w, uint32_t &h,
			uint32_t &recw, uint32_t &recy,
			bool &invx, bool &invy) {

	unsigned ucs[256], i, ucslen;
	unsigned len = strlen(txt);
	i = 0;
	ucslen = 0;
	while (len > 0 && txt[i]) {
		size_t ret = rfb::utf8ToUCS4(&txt[i], len, &ucs[ucslen]);
		i += ret;
		len -= ret;
		ucslen++;
	}

	FT_Matrix mat;
	FT_Vector pen;

	angle2mat(mat);

	pen.x = 0;
	pen.y = 0;

	FT_BBox firstbox, lastbox;

	for (i = 0; i < ucslen; i++) {
		FT_Set_Transform(face, &mat, &pen);

		if (FT_Load_Char(face, ucs[i], FT_LOAD_DEFAULT))
			continue;

		if (i == 0) {
			FT_Glyph glyph;

			FT_Get_Glyph(face->glyph, &glyph);
			FT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_PIXELS, &firstbox);
			FT_Done_Glyph(glyph);

			// recommended y; if the angle is steep enough, use the X bearing
			#define EDGE 22
			const int angle = abs(Server::DLP_WatermarkTextAngle);
			if ((angle > (45 + EDGE) && angle < (135 - EDGE)) ||
				(angle > (225 + EDGE) && angle < (315 - EDGE)))
				recy = face->glyph->metrics.horiBearingX >> 6;
			else
				recy = face->glyph->metrics.horiBearingY >> 6;
			#undef EDGE
		} else if (i == ucslen - 1) {
			FT_Glyph glyph;

			FT_Get_Glyph(face->glyph, &glyph);
			FT_Glyph_Get_CBox(glyph, FT_GLYPH_BBOX_PIXELS, &lastbox);
			FT_Done_Glyph(glyph);
		}

		if (i != ucslen - 1) {
			pen.x += face->glyph->advance.x;
			pen.y += face->glyph->advance.y;
		}
	}

	// recommended width, used when X is inverted
	recw = face->size->metrics.max_advance >> 6;

	// The used area is an union of first box, last box, and their relative distance
	invx = pen.x < 0;
	invy = pen.y > 0;

	w = (firstbox.xMax - firstbox.xMin) + (lastbox.xMax - lastbox.xMin) + abs(pen.x >> 6);
	h = (firstbox.yMax - firstbox.yMin) + (lastbox.yMax - lastbox.yMin) + abs(pen.y >> 6);
}

static bool drawtext(const char fmt[], const int16_t utcOff, const char fontpath[],
			const uint8_t fontsize) {
	char buf[PATH_MAX];

	if (!ft) {
		if (FT_Init_FreeType(&ft))
			abort();
		if (fontpath[0]) {
			if (FT_New_Face(ft, fontpath, 0, &face))
				abort();
		} else {
			if (FT_New_Memory_Face(ft, font_otf, sizeof(font_otf), 0, &face))
				abort();
		}
		FT_Set_Pixel_Sizes(face, fontsize, fontsize);
	}

	time_t now = lastUpdate = time(NULL);
	now += utcOff * 60;

	struct tm *tm = gmtime(&now);
	size_t len = strftime(buf, PATH_MAX, fmt, tm);
	if (!len)
		return false;

	free(watermarkInfo.src);
	if (Server::DLP_WatermarkTextAngle) {
		uint32_t w, h, recw, recy = fontsize;
		bool invx, invy;
		angledsize(buf, w, h, recw, recy, invx, invy);

		// The max is because a rotated text with the time can change size.
		// With the max op, at least it will only grow instead of bouncing.
		w = __rfbmax(w, watermarkInfo.w);
		h = __rfbmax(h, watermarkInfo.h);

		watermarkInfo.w = w;
		watermarkInfo.h = h;
		watermarkInfo.src = (uint8_t *) calloc(w, h);

		angledstr(watermarkInfo.src, buf,
				invx ? w - recw: 0, invy ? h - recy : recy,
				w, h, w, invx, invy);
	} else {
		const uint32_t h = fontsize + 4;
		const uint32_t w = drawnwidth(buf);

		watermarkInfo.w = w;
		watermarkInfo.h = h;
		watermarkInfo.src = (uint8_t *) calloc(w, h);

		str(watermarkInfo.src, buf, 0, fontsize, w, h, w);
	}

	return true;
}

bool watermarkInit() {
	memset(&watermarkInfo, 0, sizeof(watermarkInfo_t));
	watermarkData = watermarkUnpacked = watermarkTmp = NULL;
	rw = rh = 0;

	if (!Server::DLP_WatermarkImage[0] && !Server::DLP_WatermarkText[0])
		return true;

	if (Server::DLP_WatermarkImage[0] && Server::DLP_WatermarkText[0]) {
		vlog.error("WatermarkImage and WatermarkText can't be used together");
		return false;
	}

	if (Server::DLP_WatermarkImage[0] && !loadimage(Server::DLP_WatermarkImage))
		return false;

	if (Server::DLP_WatermarkText[0] &&
		!drawtext(Server::DLP_WatermarkText,
				Server::DLP_WatermarkTimeOffset * 60 + Server::DLP_WatermarkTimeOffsetMinutes,
				Server::DLP_WatermarkFont, Server::DLP_WatermarkFontSize))
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

static void packWatermark() {
	// Take the expanded 4-bit data, filter it by the changed rects, pack
	// to shared bytes, and compress with zlib

	uint16_t x, y;
	uint8_t pix[2], cur = 0;
	uint8_t *dst = watermarkTmp;

	for (y = 0; y < rh; y++) {
		for (x = 0; x < rw; x++) {
			pix[cur] = watermarkUnpacked[y * rw + x];
			if (cur || (y == rh - 1 && x == rw - 1))
				*dst++ = pix[0] | (pix[1] << 4);

			cur ^= 1;
		}
	}

	uLong destLen = MAXW * MAXH / 2;
	if (compress2(watermarkData, &destLen, watermarkTmp, rw * rh / 2 + 1, 1) != Z_OK)
		vlog.error("Zlib compression error");

	watermarkDataLen = destLen;
}

// update the screen-size rendered watermark whenever the screen is resized
// or if using text, every frame
void VNCServerST::updateWatermark() {
	if (rw == pb->width() &&
		rh == pb->height()) {

		if (Server::DLP_WatermarkImage[0])
			return;
		if (!watermarkTextNeedsUpdate(false))
			return;
	}

	if (Server::DLP_WatermarkText[0] && watermarkTextNeedsUpdate(false)) {
		drawtext(Server::DLP_WatermarkText,
				Server::DLP_WatermarkTimeOffset * 60 + Server::DLP_WatermarkTimeOffsetMinutes,
				Server::DLP_WatermarkFont, Server::DLP_WatermarkFontSize);
	}

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

	packWatermark();

	sendWatermark = true;
}

// Limit changes to once per second
bool watermarkTextNeedsUpdate(const bool early) {
	static time_t now;

	// We're called a couple times per frame, only grab the
	// time on the first time so it doesn't change inside a frame
	if (early)
		now = time(NULL);

	return now != lastUpdate && strchr(Server::DLP_WatermarkText, '%');
}

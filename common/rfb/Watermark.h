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

#ifndef WATERMARK_H
#define WATERMARK_H

#include <stdint.h>
#include <rfb/Region.h>

struct watermarkInfo_t {
	uint8_t *src;
	uint16_t w, h;

	int16_t x, y;
	uint16_t repeat;

	uint8_t r, g, b, a;
};

extern watermarkInfo_t watermarkInfo;

bool watermarkInit();
bool watermarkTextNeedsUpdate(const bool early);

extern uint8_t *watermarkData;
extern uint32_t watermarkDataLen;

#endif

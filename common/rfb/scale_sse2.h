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

#ifndef __RFB_SCALE_SSE2_H__
#define __RFB_SCALE_SSE2_H__

#include <stdint.h>

namespace rfb {

	void SSE2_halve(const uint8_t *oldpx,
			const uint16_t tgtw, const uint16_t tgth,
			uint8_t *newpx,
			const unsigned oldstride, const unsigned newstride);

	void SSE2_scale(const uint8_t *oldpx,
			const uint16_t tgtw, const uint16_t tgth,
			uint8_t *newpx,
			const unsigned oldstride, const unsigned newstride,
			const float tgtdiff);
};

#endif

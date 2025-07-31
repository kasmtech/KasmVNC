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

#include <emmintrin.h>

#include <rfb/scale_sse2.h>

namespace rfb {

/*
static void print128(const char msg[], const __m128i v) {
	union {
		__m128i v;
		uint8_t c[16];
	} u;

	u.v = v;

	printf("%s %02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x\n",
		msg,
		u.c[0],
		u.c[1],
		u.c[2],
		u.c[3],
		u.c[4],
		u.c[5],
		u.c[6],
		u.c[7],
		u.c[8],
		u.c[9],
		u.c[10],
		u.c[11],
		u.c[12],
		u.c[13],
		u.c[14],
		u.c[15]);
}
*/

void SSE2_halve(const uint8_t *oldpx,
			const uint16_t tgtw, const uint16_t tgth,
			uint8_t *newpx,
			const unsigned oldstride, const unsigned newstride) {
	uint16_t x, y;
	const uint16_t srcw = tgtw * 2, srch = tgth * 2;
	const __m128i zero = _mm_setzero_si128();
	const __m128i shift = _mm_set_epi32(0, 0, 0, 2);
	const __m128i low = _mm_set_epi32(0, 0, 0xffffffff, 0xffffffff);
	const __m128i high = _mm_set_epi32(0xffffffff, 0xffffffff, 0, 0);

	for (y = 0; y < srch; y += 2) {
		const uint8_t * const row0 = oldpx + oldstride * y * 4;
		const uint8_t * const row1 = oldpx + oldstride * (y + 1) * 4;

		uint8_t * const dst = newpx + newstride * (y / 2) * 4;

		for (x = 0; x < srcw - 3; x += 4) {
			__m128i lo, hi, a, b, c, d;
			lo = _mm_loadu_si128((__m128i *) &row0[x * 4]);
			hi = _mm_loadu_si128((__m128i *) &row1[x * 4]);

			a = _mm_unpacklo_epi8(lo, zero);
			b = _mm_unpackhi_epi8(lo, zero);
			c = _mm_unpacklo_epi8(hi, zero);
			d = _mm_unpackhi_epi8(hi, zero);

			a = _mm_add_epi16(a, c);
			b = _mm_add_epi16(b, d);

			c = _mm_srli_si128(a, 8);
			a = _mm_and_si128(a, low);
			a = _mm_add_epi16(a, c);

			d = _mm_slli_si128(b, 8);
			b = _mm_and_si128(b, high);
			b = _mm_add_epi16(b, d);

			a = _mm_add_epi16(a, b);

			a = _mm_srl_epi16(a, shift);
			a = _mm_packus_epi16(a, zero);

			_mm_storel_epi64((__m128i *) &dst[(x / 2) * 4], a);
		}

		for (; x < srcw; x += 2) {
			// Remainder in C
			uint8_t i;
			for (i = 0; i < 4; i++) {
				dst[(x / 2) * 4 + i] =
					(row0[x * 4 + i] +
					row0[(x + 1) * 4 + i] +
					row1[x * 4 + i] +
					row1[(x + 1) * 4 + i]) / 4;
			}
		}
	}
}

// Handles factors between 0.5 and 1.0
void SSE2_scale(const uint8_t *oldpx,
		const uint16_t tgtw, const uint16_t tgth,
		uint8_t *newpx,
		const unsigned oldstride, const unsigned newstride,
		const float tgtdiff) {

	uint16_t x, y;
	const __m128i zero = _mm_setzero_si128();
	const __m128i low = _mm_set_epi32(0, 0, 0xffffffff, 0xffffffff);
	const __m128i high = _mm_set_epi32(0xffffffff, 0xffffffff, 0, 0);
	const float invdiff = 1 / tgtdiff;

	// Calculate source dimensions from target dimensions and scaling factor
	const uint16_t srcw = (uint16_t)(tgtw * invdiff);
	const uint16_t srch = (uint16_t)(tgth * invdiff);

	for (y = 0; y < tgth; y++) {
		const float ny = y * invdiff;
		const uint16_t lowy = ny;
		const uint16_t highy = lowy + 1;

		// Handle Y-coordinate boundary case with safe fallback
		if (highy >= srch) {
			// Safe fallback: Use only the last valid row (lowy) for interpolation
			const uint16_t safe_lowy = (lowy < srch) ? lowy : srch - 1;
			const uint32_t * const row0 = (uint32_t *) (oldpx + oldstride * safe_lowy * 4);
			const uint8_t * const brow0 = (uint8_t *) row0;

			uint8_t * const dst = newpx + newstride * y * 4;

			// Process entire row with C fallback (no vertical interpolation needed)
			for (x = 0; x < tgtw; x++) {
				const float nx = x * invdiff;
				const uint16_t lowx = nx;
				const uint16_t highx = (lowx + 1 < srcw) ? lowx + 1 : lowx;
				const uint16_t right = (nx - lowx) * 256;
				const uint16_t left = 256 - right;

				uint8_t i;
				for (i = 0; i < 4; i++) {
					// Only horizontal interpolation since we're at bottom edge
					uint32_t val = brow0[lowx * 4 + i] * left;
					val += brow0[highx * 4 + i] * right;
					dst[x * 4 + i] = val >> 8;
				}
			}
			continue; // Skip to next row
		}

		// Normal case: both lowy and highy are valid
		const uint16_t bot = (ny - lowy) * 256;
		const uint16_t top = 256 - bot;
		const uint32_t * const row0 = (uint32_t *) (oldpx + oldstride * lowy * 4);
		const uint32_t * const row1 = (uint32_t *) (oldpx + oldstride * highy * 4);
		const uint8_t * const brow0 = (uint8_t *) row0;
		const uint8_t * const brow1 = (uint8_t *) row1;

		uint8_t * const dst = newpx + newstride * y * 4;

		const __m128i vertmul = _mm_set1_epi16(top);
		const __m128i vertmul2 = _mm_set1_epi16(bot);

		for (x = 0; x < tgtw - 1; x += 2) {
			const float nx[2] = {
				x * invdiff,
				(x + 1) * invdiff,
			};
			const uint16_t lowx[2] =  {
				(uint16_t) nx[0],
				(uint16_t) nx[1],
			};
			const uint16_t highx[2] = {
				(uint16_t) (lowx[0] + 1),
				(uint16_t) (lowx[1] + 1),
			};

			// Critical bounds check for X coordinates
			if (highx[0] >= srcw || highx[1] >= srcw) {
				// Fall back to C implementation for boundary pixels
				for (int i = 0; i < 2 && (x + i) < tgtw; i++) {
					const float nx_safe = (x + i) * invdiff;
					const uint16_t lowx_safe = nx_safe;
					const uint16_t highx_safe = (lowx_safe + 1 < srcw) ? lowx_safe + 1 : lowx_safe;
					const uint16_t right_safe = (nx_safe - lowx_safe) * 256;
					const uint16_t left_safe = 256 - right_safe;

					uint8_t j;
					uint32_t val, val2;
					for (j = 0; j < 4; j++) {
						val = brow0[lowx_safe * 4 + j] * left_safe;
						val += brow0[highx_safe * 4 + j] * right_safe;
						val >>= 8;

						val2 = brow1[lowx_safe * 4 + j] * left_safe;
						val2 += brow1[highx_safe * 4 + j] * right_safe;
						val2 >>= 8;

						dst[(x + i) * 4 + j] = (val * top + val2 * bot) >> 8;
					}
				}
				x++; // Skip the second pixel since we processed both
				continue;
			}

			const uint16_t right[2] = {
				(uint16_t) ((nx[0] - lowx[0]) * 256),
				(uint16_t) ((nx[1] - lowx[1]) * 256),
			};
			const uint16_t left[2] = {
				(uint16_t) (256 - right[0]),
				(uint16_t) (256 - right[1]),
			};

			const __m128i horzmul = _mm_set_epi16(
				right[0],
				right[0],
				right[0],
				right[0],
				left[0],
				left[0],
				left[0],
				left[0]
			);
			const __m128i horzmul2 = _mm_set_epi16(
				right[1],
				right[1],
				right[1],
				right[1],
				left[1],
				left[1],
				left[1],
				left[1]
			);

			__m128i lo, hi, a, b, c, d;

			// Now safe to access these indices - bounds already checked
			lo = _mm_setr_epi32(row0[lowx[0]],
						row0[highx[0]],
						row0[lowx[1]],
						row0[highx[1]]);
			hi = _mm_setr_epi32(row1[lowx[0]],
						row1[highx[0]],
						row1[lowx[1]],
						row1[highx[1]]);

			a = _mm_unpacklo_epi8(lo, zero);
			b = _mm_unpackhi_epi8(lo, zero);
			c = _mm_unpacklo_epi8(hi, zero);
			d = _mm_unpackhi_epi8(hi, zero);

			a = _mm_mullo_epi16(a, vertmul);
			b = _mm_mullo_epi16(b, vertmul);
			c = _mm_mullo_epi16(c, vertmul2);
			d = _mm_mullo_epi16(d, vertmul2);

			a = _mm_add_epi16(a, c);
			a = _mm_srli_epi16(a, 8);
			b = _mm_add_epi16(b, d);
			b = _mm_srli_epi16(b, 8);

			a = _mm_mullo_epi16(a, horzmul);
			b = _mm_mullo_epi16(b, horzmul2);

			lo = _mm_srli_si128(a, 8);
			a = _mm_and_si128(a, low);
			a = _mm_add_epi16(a, lo);

			hi = _mm_slli_si128(b, 8);
			b = _mm_and_si128(b, high);
			b = _mm_add_epi16(b, hi);

			a = _mm_add_epi16(a, b);
			a = _mm_srli_epi16(a, 8);

			a = _mm_packus_epi16(a, zero);

			_mm_storel_epi64((__m128i *) &dst[x * 4], a);
		}

		for (; x < tgtw; x++) {
			// Remainder in C with bounds checking
			const float nx = x * invdiff;
			const uint16_t lowx = nx;
			const uint16_t highx = (lowx + 1 < srcw) ? lowx + 1 : lowx;
			const uint16_t right = (nx - lowx) * 256;
			const uint16_t left = 256 - right;

			uint8_t i;
			uint32_t val, val2;
			for (i = 0; i < 4; i++) {
				val = brow0[lowx * 4 + i] * left;
				val += brow0[highx * 4 + i] * right;
				val >>= 8;

				val2 = brow1[lowx * 4 + i] * left;
				val2 += brow1[highx * 4 + i] * right;
				val2 >>= 8;

				dst[x * 4 + i] =
					(val * top + val2 * bot) >> 8;
			}
		}
	}
}

}; // namespace rfb

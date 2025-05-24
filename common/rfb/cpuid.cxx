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

#include "cpuid.h"
#include <cstdint>
#include "LogWriter.h"

static uint32_t cpuid[4] = {};
static uint32_t extcpuid[4] = {};

static void getcpuid() {
	if (cpuid[0])
		return;

#if defined(__x86_64__) || defined(__i386__)
	uint32_t eax, ecx = 0;

	eax = 1; // normal feature bits

	__asm__ __volatile__(
		"cpuid\n\t"
		: "=a"(cpuid[0]), "=b"(cpuid[1]), "=c"(cpuid[2]), "=d"(cpuid[3])
		: "0"(eax), "2"(ecx)
	);

	eax = 7; // ext feature bits
	ecx = 0;

	__asm__ __volatile__(
		"cpuid\n\t"
		: "=a"(extcpuid[0]), "=b"(extcpuid[1]), "=c"(extcpuid[2]), "=d"(extcpuid[3])
		: "0"(eax), "2"(ecx)
	);
#endif
}

namespace rfb {

bool supportsSSE2() {
	getcpuid();
#if defined(__x86_64__) || defined(__i386__)
	#define bit_SSE2        (1 << 26)
	return cpuid[3] & bit_SSE2;
#endif
	return false;
}

bool supportsAVX512f() {
	getcpuid();
#if defined(__x86_64__) || defined(__i386__)
	#define bit_AVX512f        (1 << 16)
	return extcpuid[1] & bit_AVX512f;
#endif
	return false;
}

}; // namespace rfb

namespace cpu_info {
    static rfb::LogWriter log("CpuFeatures");
    inline CpuFeatures::CpuFeatures()
    {
        if (!cpuid_present())
        {
            log.error("CPU does not support CPUID.");
            return;
        }

        cpu_raw_data_t raw{};

        if (cpuid_get_raw_data(&raw) < 0)
        {
            log.error("Cannot get CPUID raw data.");
            return;
        }

        if (cpu_identify(&raw, &data) < 0)
        {
            log.error("Cannot identify CPU.");
        }
    }
} // namespace cpu_info

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

#ifndef __RFB_CPUID_H__
#define __RFB_CPUID_H__

#include <algorithm>
#include <libcpuid/libcpuid.h>

namespace cpu_info {
    //using namespace cpu_features;
    //static const X86Info info = GetX86Info();
   // static const X86Microarchitecture uarch = GetX86Microarchitecture(&info);
    //static const bool has_fast_avx = info.features.avx && uarch != INTEL_SNB;

    bool supportsSSE2();
    bool supportsAVX512f();

    class CpuFeatures {
        cpu_id_t data{};
        CpuFeatures();

    public:
        CpuFeatures(const CpuFeatures &) = delete;
        CpuFeatures &operator=(const CpuFeatures &) = delete;
        CpuFeatures(CpuFeatures &&) = delete;
        CpuFeatures &operator=(CpuFeatures &&) = delete;

        static CpuFeatures &get()
        {
            static CpuFeatures instance{};
            return instance;
        }

        [[nodiscard]] bool has_sse2() const { return data.flags[CPU_FEATURE_SSE2]; }

        [[nodiscard]] bool has_sse4_1() const { return data.flags[CPU_FEATURE_SSE4_1]; }

        [[nodiscard]] bool has_sse4_2() const { return data.flags[CPU_FEATURE_SSE4_2]; }

        [[nodiscard]] bool has_sse4a() const { return data.flags[CPU_FEATURE_SSE4A]; }

        [[nodiscard]] bool has_avx() const { return data.flags[CPU_FEATURE_AVX]; }

        [[nodiscard]] bool has_avx2() const { return data.flags[CPU_FEATURE_AVX2]; }

        [[nodiscard]] bool has_avx512f() const { return data.flags[CPU_FEATURE_AVX512F]; }

        [[nodiscard]] bool has_smt() const { return get_total_cpu_count() > get_cores_count(); }

        [[nodiscard]] uint16_t get_total_cpu_count() const { return std::max(1, data.total_logical_cpus); }

        [[nodiscard]] uint16_t get_cores_count() const { return std::max(1, data.num_cores); }
    };

    inline static const bool has_sse2 = CpuFeatures::get().has_sse2();
    inline static const bool has_sse4_1 = CpuFeatures::get().has_sse4_1();
    inline static const bool has_sse4_2 = CpuFeatures::get().has_sse4_2();
    inline static const bool has_sse4a = CpuFeatures::get().has_sse4a();
    inline static const bool has_avx = CpuFeatures::get().has_avx();
    inline static const bool has_avx2 = CpuFeatures::get().has_avx2();
    inline static const bool has_avx512f = CpuFeatures::get().has_avx512f();
    inline static const uint16_t cores_count = CpuFeatures::get().get_cores_count();
    inline static const uint16_t total_cpu_count = CpuFeatures::get().get_total_cpu_count();
}; // namespace cpu_info

#endif

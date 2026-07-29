#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <statgrab.h>

struct mem_stats_t {
    uint64_t total;
    uint64_t free;
    uint64_t used;
    uint64_t cached;
};

struct disk_stats_t {
    std::string disk_name;
    uint64_t bytes_read;
    uint64_t bytes_written;
    double bytes_read_per_sec;
    double bytes_written_per_sec;
    double iowait;
};

class SystemStats {
    sg_mem_stats *mem{};
    sg_cpu_percents *cpu{};
    sg_disk_io_stats *disk{};
    size_t dev_count{};

public:
    SystemStats() {
        sg_init(true);
        cpu = sg_get_cpu_percents(nullptr);
        disk = sg_get_disk_io_stats(&dev_count);
    }

    SystemStats(const SystemStats &) = delete;

    SystemStats &operator=(const SystemStats &) = delete;

    SystemStats(SystemStats &&) = delete;

    SystemStats &operator=(SystemStats &&) = delete;

    ~SystemStats() {
        sg_shutdown();
    }

    mem_stats_t get_mem_stats() {
        mem = sg_get_mem_stats(nullptr);
        if (!mem)
            return {};

        return {
            .total = mem->total, .free = mem->free, .used = mem->used, .cached = mem->cache
        };
    }

    std::vector<disk_stats_t> get_io_stats() {
        size_t diff_count = 0;
        const auto *diff = sg_get_disk_io_stats_diff(&diff_count);
        const auto *curr = sg_get_disk_io_stats(&dev_count);
        cpu = sg_get_cpu_percents(nullptr);

        if (!diff || !curr)
            return {};

        const size_t count = std::min(diff_count, dev_count);

        std::vector<disk_stats_t> stats;
        stats.reserve(count);

        const double iowait_value = cpu && !std::isnan(cpu->iowait) ? cpu->iowait : 0.0;

        for (size_t i = 0; i < count; i++) {
            double read_per_sec = 0.0, write_per_sec = 0.0;
            if (const auto systime = static_cast<double>(diff[i].systime); systime > 0) {
                read_per_sec = static_cast<double>(diff[i].read_bytes) / systime;
                write_per_sec = static_cast<double>(diff[i].write_bytes) / systime;
            }

            stats.emplace_back(diff[i].disk_name, curr[i].read_bytes, curr[i].write_bytes, read_per_sec, write_per_sec,
                               iowait_value);
        }

        return stats;
    }

    static double get_cpu_usage() {
        const auto cpu = sg_get_cpu_percents(nullptr);

        if (!cpu || std::isnan(cpu->idle))
            return 0.0;

        return 100. - cpu->idle;
    }
};

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <statgrab.h>
#include <os/CgroupStats.h>

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
        if (const auto limits = CgroupStats::get_limits(); limits.has_mem_limit)
            return CgroupStats::get_mem_stats(limits.mem_limit);

        mem = sg_get_mem_stats(nullptr);
        if (!mem)
            return {};

        return {
            .total = mem->total, .free = mem->free, .used = mem->used, .cached = mem->cache
        };
    }

    std::vector<disk_stats_t> get_io_stats();

    static double get_cpu_usage() {
        const auto cpu = sg_get_cpu_percents(nullptr);

        if (!cpu || std::isnan(cpu->idle))
            return 0.0;

        return 100. - cpu->idle;
    }
};

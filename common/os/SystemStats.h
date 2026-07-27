#pragma once
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <statgrab.h>

struct cpu_stats_t {
    int64_t user;
    int64_t nice;
    int64_t system;
    int64_t idle;
    int64_t iowait;
    int64_t irq;
    int64_t softirq;
    int64_t steal;
    int64_t guest;

    [[nodiscard]] int64_t total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal + guest;
    }

    [[nodiscard]] bool valid() const {
        return user || nice || system || idle || iowait || irq || softirq || steal || guest;
    }
};

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

    static cpu_stats_t read_cpu_stats() {
        cpu_stats_t stats{};

        const auto fd = open("/proc/stat", O_RDONLY);
        if (fd == -1)
            return stats;

        char buf[1024];
        const auto n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n <= 0)
            return stats;

        buf[n] = '\0';

        // Skip "cpu" prefix
        const auto *p = seek(buf);

        // Manual parsing: assume values are space-separated numbers
        stats.user = atoll(p);
        p = seek(p);

        stats.nice = atoll(++p);
        p = seek(p);

        stats.system = atoll(++p);
        p = seek(p);

        stats.idle = atoll(++p);
        p = seek(p);

        stats.iowait = atoll(++p);
        p = seek(p);

        stats.irq = atoll(++p);
        p = seek(p);

        stats.softirq = atoll(++p);
        p = seek(p);

        stats.steal = atoll(++p);
        p = seek(p);

        stats.guest = atoll(++p);

        return stats;
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
        // const auto delta = static_cast<double>(now.total() - last_cpu_stats.total());
        // const auto idle = static_cast<double>(now.idle - last_cpu_stats.idle);
        // const auto used = delta - idle;
        //
        // if (delta > 0)
        //     return 1000 * used / delta;
        //
        // return 0;

        if (!cpu || std::isnan(cpu->idle))
            return 0.0;

        return 100. - cpu->idle;
    }

    static const char *seek(const char *ptr) {
        while (*ptr && *ptr != ' ' && *ptr != '\t' && *ptr != '\n')
            ++ptr;

        return ptr;
    }
};

#pragma once
#include <cstdint>
#include <cstdlib>
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
        return !user && !nice && !system && !idle && !iowait && !irq && !softirq && !steal && !guest;
    }
};

struct mem_stats_t {
    uint64_t total;
    uint64_t free;
    uint64_t used;
    uint64_t cached;
};

struct disk_stats_t {
    std::string_view disk_name;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t iowait;
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

        return {
            .total = mem->total, .free = mem->free, .used = mem->cache, .cached = mem->cache
        };
    }

    std::vector<disk_stats_t> get_io_stats() {
        auto *curr = sg_get_disk_io_stats_diff(&dev_count);
        cpu = sg_get_cpu_percents(nullptr);

        std::vector<disk_stats_t> stats;
        stats.reserve(dev_count);

        double r_s = 0.0, w_s = 0.0;
        double rkB_s = 0.0, wkB_s = 0.0;

        for (size_t i = 0; i < dev_count; i++) {
            const auto *device_stats = &curr[i];

            stats.emplace_back(device_stats->disk_name, device_stats->read_bytes, device_stats->write_bytes, cpu->iowait);
        }

        return stats;
    }

    static uint64_t get_cpu_usage(const cpu_stats_t &last, const cpu_stats_t &now) {
        const auto delta = now.total() - last.total();
        const auto idle = now.idle - last.idle;
        const auto used = delta - idle;

        return used / delta * 100;
    }

    static const char *seek(const char *ptr) {
        while (*ptr && *ptr != ' ' && *ptr != '\t' && *ptr != '\n')
            ++ptr;

        return ptr;
    }
};

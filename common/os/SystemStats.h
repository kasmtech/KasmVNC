#pragma once
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

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

    int64_t total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal + guest;
    }

    bool valid() const {
        return !user && !nice && !system && !idle && !iowait && !irq && !softirq && !steal && !guest;
    }
};

struct mem_stats_t {
    uint64_t total;
    uint64_t free;
    uint64_t cached;
    uint64_t buffers;
    uint64_t shared;
    uint64_t slab;
};

struct SystemStats {
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
        char *p = buf + 4;

        // Manual parsing: assume values are space-separated numbers
        stats.user = atoll(p);
        seek(p);

        stats.nice = atoll(++p);
        seek(p);

        stats.system = atoll(++p);
        seek(p);

        stats.idle = atoll(++p);
        seek(p);

        stats.iowait = atoll(++p);
        seek(p);

        stats.irq = atoll(++p);
        seek(p);

        stats.softirq = atoll(++p);
        seek(p);

        stats.steal = atoll(++p);
        seek(p);

        stats.guest = atoll(++p);
        seek(p);

        return stats;
    }

    static uint64_t get_cpu_usage(const cpu_stats_t &last, const cpu_stats_t &now) {
        const auto delta = now.total() - last.total();
        const auto idle = now.idle - last.idle;
        const auto used = delta - idle;

        return used / delta * 100;
    }

    static void seek(const char *ptr) {
        while (*ptr && *ptr != ' ')
            ++ptr;
    }
};

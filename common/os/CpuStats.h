#pragma once

#include <os/CgroupStats.h>
#include <os/SysfsReader.h>

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>

struct cgroup_cpu_stats_t {
    bool available{false};
    double usage_percent{0.0};
    double availability_percent{100.0};
    double effective_cores{0.0};
    uint64_t usage_usec{0};
    uint64_t user_usec{0};
    uint64_t system_usec{0};
    bool has_throttling{false};
    uint64_t nr_periods{0};
    uint64_t nr_throttled{0};
    uint64_t throttled_usec{0};
    double throttled_percent{0.0};
};

class CpuStats {
public:
    static cgroup_cpu_stats_t get_cpu_stats(const cgroup_limits_t &limits);

private:
    struct raw_sample_t {
        uint64_t usage_usec{0};
        uint64_t user_usec{0};
        uint64_t system_usec{0};
        uint64_t nr_periods{0};
        uint64_t nr_throttled{0};
        uint64_t throttled_usec{0};
    };

    struct cpu_usage_sample_t {
        bool available{false};
        uint64_t usage_usec{0};
        uint64_t throttled_usec{0};
        timespec at;

        cpu_usage_sample_t() : at{} {
        }
    };

    inline static const SysfsReader<std::string> v2_cpu_stat{"/sys/fs/cgroup/cpu.stat"};
    inline static const SysfsReader<uint64_t> v1_cpuacct_usage{"/sys/fs/cgroup/cpuacct/cpuacct.usage"};
    inline static const SysfsReader<std::string> v1_cpu_stat{"/sys/fs/cgroup/cpu/cpu.stat"};

    inline static std::mutex usage_mutex;
    inline static cpu_usage_sample_t previous_usage;

    static double effective_cores(const cgroup_limits_t &limits, uint32_t host_cpus);

    static cgroup_cpu_stats_t make_stats(const cgroup_limits_t &limits, const raw_sample_t &sample);

    static cgroup_cpu_stats_t read_v1(const cgroup_limits_t &limits);

    static cgroup_cpu_stats_t read_v2(const cgroup_limits_t &limits);
};

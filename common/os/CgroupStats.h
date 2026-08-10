#pragma once
#include <os/SysfsReader.h>

#include <cstdint>
#include <string>
#include <string_view>

struct cgroup_limits_t {
    bool has_mem_limit{false};
    uint64_t mem_limit{0};
    bool has_cpu_limit{false};
    double cpu_limit_cores{0.0};
    bool has_cpu_weight{false};
    uint64_t cpu_weight{0};
    bool has_cpu_affinity{false};
    std::string cpu_affinity;
};

struct mem_stats_t;

class CgroupStats {
    // v2 readers
    inline static const SysfsReader<std::string> v2_mem_max{"/sys/fs/cgroup/memory.max"};
    inline static const SysfsReader<std::string> v2_cpu_max{"/sys/fs/cgroup/cpu.max"};
    inline static const SysfsReader<uint64_t> v2_cpu_weight{"/sys/fs/cgroup/cpu.weight"};
    inline static const SysfsReader<std::string> v2_cpuset{"/sys/fs/cgroup/cpuset.cpus.effective"};

    // v1 readers
    inline static const SysfsReader<uint64_t> v1_mem_limit{"/sys/fs/cgroup/memory/memory.limit_in_bytes"};
    inline static const SysfsReader<int64_t> v1_cpu_quota{"/sys/fs/cgroup/cpu/cpu.cfs_quota_us"};
    inline static const SysfsReader<int64_t> v1_cpu_period{"/sys/fs/cgroup/cpu/cpu.cfs_period_us"};
    inline static const SysfsReader<uint64_t> v1_cpu_shares{"/sys/fs/cgroup/cpu/cpu.shares"};
    inline static const SysfsReader<std::string> v1_cpuset{"/sys/fs/cgroup/cpuset/cpuset.cpus"};

    // mem stats readers
    inline static const SysfsReader<uint64_t> mem_current{"/sys/fs/cgroup/memory.current"};
    inline static const SysfsReader<uint64_t> mem_usage{"/sys/fs/cgroup/memory/memory.usage_in_bytes"};
    inline static const SysfsReader<std::string> mem_stat_v2{"/sys/fs/cgroup/memory.stat"};
    inline static const SysfsReader<std::string> mem_stat_v1{"/sys/fs/cgroup/memory/memory.stat"};

    static bool parse_uint64(std::string_view value, uint64_t &out);

    static bool parse_mem_max_v2(const std::string &value, uint64_t &out);

    static bool parse_cpu_max_v2(const std::string &line, double &cores_out);

    static void apply_cpu_weight(const SysfsReader<uint64_t> &reader, cgroup_limits_t &limits);

    static void apply_cpu_affinity(const SysfsReader<std::string> &reader, uint32_t host_cpus,
                                    cgroup_limits_t &limits);

    static cgroup_limits_t read_limits_v1();

    static cgroup_limits_t read_limits_v2();

public:
    enum class Version { none, v1, v2 };

    static Version version();

    static uint32_t host_cpu_count();

    static uint32_t count_cpu_set(std::string_view value);

    static bool try_read_uint64(const SysfsReader<uint64_t> &reader, uint64_t &out);

    static cgroup_limits_t get_limits();

    static mem_stats_t get_mem_stats(uint64_t mem_limit);
};

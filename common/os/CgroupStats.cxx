#include <os/CgroupStats.h>
#include <os/SystemStats.h>

CgroupStats::Version CgroupStats::detect_version() {
    if (SysfsReader<std::string>("/sys/fs/cgroup/cgroup.controllers").valid())
        return Version::v2;

    if (SysfsReader<std::string>("/sys/fs/cgroup/memory/memory.limit_in_bytes").valid())
        return Version::v1;

    return Version::none;
}

CgroupStats::Version CgroupStats::version() {
    static const Version v = detect_version();
    return v;
}

bool CgroupStats::parse_uint64(const std::string &value, uint64_t &out) {
    if (value.empty())
        return false;

    uint64_t val = 0;
    for (const char c: value) {
        if (c < '0' || c > '9')
            return false;

        val = val * 10 + static_cast<uint64_t>(c - '0');
    }

    out = val;
    return true;
}

bool CgroupStats::parse_mem_max_v2(const std::string &value, uint64_t &out) {
    if (value.empty() || value == "max")
        return false;

    return parse_uint64(value, out);
}

bool CgroupStats::parse_cpu_max_v2(const std::string &line, double &cores_out) {
    const auto space_pos = line.find(' ');
    if (space_pos == std::string::npos)
        return false;

    const std::string quota_str = line.substr(0, space_pos);
    if (quota_str == "max")
        return false;

    uint64_t quota, period;
    if (!parse_uint64(quota_str, quota) || !parse_uint64(line.substr(space_pos + 1), period) || period == 0)
        return false;

    cores_out = static_cast<double>(quota) / static_cast<double>(period);
    return true;
}

bool CgroupStats::try_read_uint64(const SysfsReader<uint64_t> &reader, uint64_t &out) {
    if (!reader.valid())
        return false;

    const uint64_t value = reader.read();
    if (value == static_cast<uint64_t>(-1))
        return false;

    out = value;

    return true;
}

cgroup_limits_t CgroupStats::read_limits_v2() {
    cgroup_limits_t limits;

    if (uint64_t mem; parse_mem_max_v2(v2_mem_max.read(), mem)) {
        limits.has_mem_limit = true;
        limits.mem_limit = mem;
    }

    if (double cores; parse_cpu_max_v2(v2_cpu_max.read(), cores)) {
        limits.has_cpu_limit = true;
        limits.cpu_limit_cores = cores;
    }

    if (uint64_t weight; try_read_uint64(v2_cpu_weight, weight)) {
        limits.has_cpu_weight = true;
        limits.cpu_weight = weight;
    }

    if (std::string affinity = v2_cpuset.read(); !affinity.empty()) {
        limits.has_cpu_affinity = true;
        limits.cpu_affinity = std::move(affinity);
    }

    return limits;
}

cgroup_limits_t CgroupStats::read_limits_v1() {
    cgroup_limits_t limits;

    constexpr uint64_t v1_unlimited_threshold = 1ULL << 62;
    if (uint64_t mem; try_read_uint64(v1_mem_limit, mem) && mem < v1_unlimited_threshold) {
        limits.has_mem_limit = true;
        limits.mem_limit = mem;
    }

    if (const int64_t quota_us = v1_cpu_quota.read(), period_us = v1_cpu_period.read();
        quota_us > 0 && period_us > 0) {
        limits.has_cpu_limit = true;
        limits.cpu_limit_cores = static_cast<double>(quota_us) / static_cast<double>(period_us);
    }

    if (uint64_t weight; try_read_uint64(v1_cpu_shares, weight)) {
        limits.has_cpu_weight = true;
        limits.cpu_weight = weight;
    }

    if (std::string affinity = v1_cpuset.read(); !affinity.empty()) {
        limits.has_cpu_affinity = true;
        limits.cpu_affinity = std::move(affinity);
    }

    return limits;
}

cgroup_limits_t CgroupStats::get_limits() {
    switch (version()) {
        case Version::v2:
            return read_limits_v2();
        case Version::v1:
            return read_limits_v1();
        default:
            return {};
    }
}

mem_stats_t CgroupStats::get_mem_stats(const uint64_t mem_limit) {
    uint64_t used = 0;
    if (!try_read_uint64(mem_current, used))
        try_read_uint64(mem_usage, used);

    const uint64_t free = used < mem_limit ? mem_limit - used : 0;

    return {.total = mem_limit, .free = free, .used = used, .cached = 0};
}

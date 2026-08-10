#include <os/CgroupStats.h>
#include <os/StatFileParser.h>
#include <os/SystemStats.h>

#include <unistd.h>
#include <utility>

CgroupStats::Version CgroupStats::version() {
    auto detect_version = [] {
        if (SysfsReader<std::string>("/sys/fs/cgroup/cgroup.controllers").valid())
            return Version::v2;

        if (SysfsReader<std::string>("/sys/fs/cgroup/memory/memory.limit_in_bytes").valid())
            return Version::v1;

        return Version::none;
    };

    static const Version v = detect_version();
    return v;
}

bool CgroupStats::parse_uint64(const std::string_view value, uint64_t &out) {
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

uint32_t CgroupStats::count_cpu_set(const std::string_view value) {
    uint32_t count = 0;
    size_t pos = 0;

    while (pos < value.size()) {
        auto comma_pos = value.find(',', pos);
        if (comma_pos == std::string_view::npos)
            comma_pos = value.size();

        const std::string_view range = value.substr(pos, comma_pos - pos);
        pos = comma_pos + 1;

        if (range.empty())
            continue;

        uint64_t start = 0, end = 0;
        const auto dash_pos = range.find('-');
        if (dash_pos == std::string_view::npos) {
            if (parse_uint64(range, start))
                count++;
            continue;
        }

        if (!parse_uint64(range.substr(0, dash_pos), start) || !parse_uint64(range.substr(dash_pos + 1), end) ||
            end < start) {
            continue;
        }

        count += static_cast<uint32_t>(end - start + 1);
    }

    return count;
}

uint32_t CgroupStats::host_cpu_count() {
    static const uint32_t count = [] {
        const auto n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? static_cast<uint32_t>(n) : 1;
    }();

    return count;
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

void CgroupStats::apply_cpu_weight(const SysfsReader<uint64_t> &reader, cgroup_limits_t &limits) {
    if (uint64_t weight; try_read_uint64(reader, weight)) {
        limits.has_cpu_weight = true;
        limits.cpu_weight = weight;
    }
}

void CgroupStats::apply_cpu_affinity(const SysfsReader<std::string> &reader, const uint32_t host_cpus,
                                     cgroup_limits_t &limits) {
    if (std::string affinity = reader.read(); !affinity.empty()) {
        const auto affinity_count = count_cpu_set(affinity);
        limits.has_cpu_affinity = affinity_count > 0 && affinity_count < host_cpus;
        limits.cpu_affinity = std::move(affinity);
    }
}

cgroup_limits_t CgroupStats::read_limits_v2() {
    cgroup_limits_t limits;
    const auto host_cpus = host_cpu_count();

    if (uint64_t mem; parse_mem_max_v2(v2_mem_max.read(), mem)) {
        limits.has_mem_limit = true;
        limits.mem_limit = mem;
    }

    if (double cores; parse_cpu_max_v2(v2_cpu_max.read(), cores)) {
        limits.has_cpu_limit = true;
        limits.cpu_limit_cores = cores;
    }

    apply_cpu_weight(v2_cpu_weight, limits);
    apply_cpu_affinity(v2_cpuset, host_cpus, limits);

    return limits;
}

cgroup_limits_t CgroupStats::read_limits_v1() {
    cgroup_limits_t limits;
    const auto host_cpus = host_cpu_count();

    constexpr uint64_t v1_unlimited_threshold = 1ULL << 62;
    if (uint64_t mem; try_read_uint64(v1_mem_limit, mem) && mem < v1_unlimited_threshold) {
        limits.has_mem_limit = true;
        limits.mem_limit = mem;
    }

    if (const int64_t quota_us = v1_cpu_quota.read(), period_us = v1_cpu_period.read(); quota_us > 0 && period_us > 0) {
        limits.has_cpu_limit = true;
        limits.cpu_limit_cores = static_cast<double>(quota_us) / static_cast<double>(period_us);
    }

    apply_cpu_weight(v1_cpu_shares, limits);
    apply_cpu_affinity(v1_cpuset, host_cpus, limits);

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
    uint64_t cached = stat_field(mem_stat_v2.read(), "file");
    if (cached == 0)
        cached = stat_field(mem_stat_v1.read(), "total_cache");

    return {.total = mem_limit, .free = free, .used = used, .cached = cached};
}

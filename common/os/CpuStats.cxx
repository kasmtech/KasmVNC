#include <os/CpuStats.h>
#include <os/StatFileParser.h>
#include <algorithm>
#include <iterator>

double CpuStats::effective_cores(const cgroup_limits_t &limits, const uint32_t host_cpus) {
    if (limits.has_cpu_limit)
        return limits.cpu_limit_cores;

    if (limits.has_cpu_affinity) {
        const auto affinity_count = CgroupStats::count_cpu_set(limits.cpu_affinity);
        if (affinity_count > 0)
            return static_cast<double>(affinity_count);
    }

    return static_cast<double>(host_cpus);
}

cgroup_cpu_stats_t CpuStats::make_stats(const cgroup_limits_t &limits, const raw_sample_t &sample) {
    cgroup_cpu_stats_t stats;
    stats.available = true;
    stats.usage_usec = sample.usage_usec;
    stats.user_usec = sample.user_usec;
    stats.system_usec = sample.system_usec;
    stats.nr_periods = sample.nr_periods;
    stats.nr_throttled = sample.nr_throttled;
    stats.throttled_usec = sample.throttled_usec;
    stats.has_throttling = sample.nr_periods > 0 || sample.nr_throttled > 0 || sample.throttled_usec > 0;

    const auto host_cpus = CgroupStats::host_cpu_count();
    stats.effective_cores = effective_cores(limits, host_cpus);
    stats.availability_percent =
            std::clamp(stats.effective_cores / static_cast<double>(host_cpus) * 100.0, 0.0, 100.0);

    struct timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);

    std::lock_guard lock(usage_mutex);

    if (previous_usage.available && sample.usage_usec >= previous_usage.usage_usec) {
        const int64_t elapsed_usec_raw = (now.tv_sec - previous_usage.at.tv_sec) * 1000000LL +
                                         (now.tv_nsec - previous_usage.at.tv_nsec) / 1000LL;

        if (elapsed_usec_raw > 0) {
            const auto elapsed_usec = static_cast<double>(elapsed_usec_raw);
            const auto usage_delta = static_cast<double>(sample.usage_usec - previous_usage.usage_usec);

            if (sample.throttled_usec >= previous_usage.throttled_usec) {
                const auto throttled_delta =
                        static_cast<double>(sample.throttled_usec - previous_usage.throttled_usec);
                stats.throttled_percent = std::clamp((throttled_delta / elapsed_usec) * 100.0, 0.0, 100.0);
            }

            if (stats.effective_cores > 0.0) {
                stats.usage_percent =
                        std::clamp(usage_delta / (elapsed_usec * stats.effective_cores) * 100.0, 0.0, 100.0);
            }
        }
    }

    previous_usage.available = true;
    previous_usage.usage_usec = sample.usage_usec;
    previous_usage.throttled_usec = sample.throttled_usec;
    previous_usage.at = now;

    return stats;
}

cgroup_cpu_stats_t CpuStats::read_v2(const cgroup_limits_t &limits) {
    const auto stat = v2_cpu_stat.read();
    if (stat.empty())
        return {};

    raw_sample_t sample;
    const stat_field_t fields[] = {
        {"usage_usec", &sample.usage_usec},
        {"user_usec", &sample.user_usec},
        {"system_usec", &sample.system_usec},
        {"nr_periods", &sample.nr_periods},
        {"nr_throttled", &sample.nr_throttled},
        {"throttled_usec", &sample.throttled_usec},
    };
    parse_stat_fields(stat, fields, std::size(fields));

    return make_stats(limits, sample);
}

cgroup_cpu_stats_t CpuStats::read_v1(const cgroup_limits_t &limits) {
    uint64_t usage_ns = 0;
    if (!CgroupStats::try_read_uint64(v1_cpuacct_usage, usage_ns))
        return {};

    const auto stat = v1_cpu_stat.read();
    uint64_t nr_periods = 0, nr_throttled = 0, throttled_time_ns = 0;
    const stat_field_t fields[] = {
        {"nr_periods", &nr_periods},
        {"nr_throttled", &nr_throttled},
        {"throttled_time", &throttled_time_ns},
    };
    parse_stat_fields(stat, fields, std::size(fields));

    return make_stats(limits, {
                          .usage_usec = usage_ns / 1000,
                          .nr_periods = nr_periods,
                          .nr_throttled = nr_throttled,
                          .throttled_usec = throttled_time_ns / 1000
                      });
}

cgroup_cpu_stats_t CpuStats::get_cpu_stats(const cgroup_limits_t &limits) {
    switch (CgroupStats::version()) {
        case CgroupStats::Version::v2:
            return read_v2(limits);
        case CgroupStats::Version::v1:
            return read_v1(limits);
        default:
            return {};
    }
}

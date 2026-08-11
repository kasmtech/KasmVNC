#include <os/SystemStats.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

bool is_pseudo_device(const std::string &name) {
    for (const auto *prefix: {"loop", "ram", "zram", "dm-"}) {
        if (name.starts_with(prefix))
            return true;
    }
    return false;
}

std::string parent_disk_name(const std::string &name) {
    if (name.starts_with("nvme") || name.starts_with("mmcblk")) {
        const auto p_pos = name.rfind('p');
        if (p_pos == std::string::npos || p_pos + 1 >= name.size())
            return {};

        if (!std::all_of(
            name.begin() + p_pos + 1, name.end(), [](const unsigned char c) { return std::isdigit(c); }))
            return {};
        return name.substr(0, p_pos);
    }

    const auto digits_start = name.find_last_not_of("0123456789") + 1;
    if (digits_start > 0 && digits_start < name.size())
        return name.substr(0, digits_start);

    return {};
}

std::vector<disk_stats_t> SystemStats::get_io_stats() {
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
        if (is_pseudo_device(diff[i].disk_name))
            continue;

        double read_per_sec = 0.0, write_per_sec = 0.0;
        if (const auto systime = static_cast<double>(diff[i].systime); systime > 0) {
            read_per_sec = static_cast<double>(diff[i].read_bytes) / systime;
            write_per_sec = static_cast<double>(diff[i].write_bytes) / systime;
        }

        stats.emplace_back(
            diff[i].disk_name, curr[i].read_bytes, curr[i].write_bytes, read_per_sec, write_per_sec, iowait_value);
    }

    std::vector<std::string> disk_names;
    disk_names.reserve(stats.size());
    for (const auto &s: stats)
        disk_names.push_back(s.disk_name);

    std::erase_if(stats, [&disk_names](const disk_stats_t &s) {
        const auto parent = parent_disk_name(s.disk_name);
        return !parent.empty() && std::find(disk_names.begin(), disk_names.end(), parent) != disk_names.end();
    });

    return stats;
}

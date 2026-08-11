#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

struct stat_field_t {
    const char *key;
    uint64_t *out;
};

inline void parse_stat_fields(const std::string_view stats, const stat_field_t *fields, const size_t count) {
    size_t pos = 0;

    while (pos < stats.size()) {
        auto line_end = stats.find('\n', pos);
        if (line_end == std::string_view::npos)
            line_end = stats.size();

        const auto space_pos = stats.find(' ', pos);
        if (space_pos != std::string_view::npos && space_pos < line_end) {
            const auto key = stats.substr(pos, space_pos - pos);
            size_t value_pos = space_pos + 1;
            uint64_t value = 0;
            bool has_digit = false;

            while (value_pos < line_end && stats[value_pos] >= '0' && stats[value_pos] <= '9') {
                value = value * 10 + static_cast<uint64_t>(stats[value_pos] - '0');
                has_digit = true;
                value_pos++;
            }

            if (has_digit) {
                for (size_t i = 0; i < count; ++i) {
                    if (key == fields[i].key) {
                        *fields[i].out = value;
                        break;
                    }
                }
            }
        }

        pos = line_end + 1;
    }
}

inline uint64_t stat_field(const std::string_view stats, const char *key) {
    uint64_t value = 0;
    const stat_field_t field{key, &value};
    parse_stat_fields(stats, &field, 1);
    return value;
}

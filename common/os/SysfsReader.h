#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>


template<typename T = long>
class SysfsReader {
    static constexpr bool is_string = std::is_same_v<T, std::string>;
    static_assert(is_string || std::is_integral_v<T>, "SysfsReader only supports integer types or std::string");

    int fd = -1;

public:
    explicit SysfsReader(const char *path) {
        fd = open(path, O_RDONLY);
    }

    ~SysfsReader() {
        if (fd >= 0)
            close(fd);
    }

    SysfsReader(const SysfsReader &) = delete;

    SysfsReader &operator=(const SysfsReader &) = delete;

    SysfsReader(SysfsReader &&o) noexcept : fd(o.fd) { o.fd = -1; }

    SysfsReader &operator=(SysfsReader &&o) noexcept {
        if (this != &o) {
            if (fd >= 0)
                close(fd);

            fd = o.fd;
            o.fd = -1;
        }

        return *this;
    }

    bool valid() const { return fd >= 0; }

    T read() const {
        char buf[4096];
        const ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);

        if constexpr (is_string) {
            if (n <= 0)
                return {};

            size_t len = static_cast<size_t>(n);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                len--;

            return std::string(buf, len);
        } else {
            if (n <= 0)
                return T(-1);
            buf[n] = '\0';

            using U = std::make_unsigned_t<T>;
            U val = 0;
            char *p = buf;
            bool neg = false;

            if constexpr (std::is_signed_v<T>) {
                if (*p == '-') {
                    neg = true;
                    p++;
                }
            }

            while (*p >= '0' && *p <= '9')
                val = val * 10 + static_cast<U>(*p++ - '0');

            if constexpr (std::is_signed_v<T>)
                return neg ? -static_cast<T>(val) : static_cast<T>(val);
            else
                return static_cast<T>(val);
        }
    }
};

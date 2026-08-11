#pragma once

#include <memory>
#include <mutex>

template<typename T>
class Snapshot {
    std::shared_ptr<const T> m_current;
    std::mutex m_write_mutex;

public:
    explicit Snapshot(T value) {
        std::atomic_store(&m_current, std::make_shared<const T>(std::move(value)));
    }

    std::shared_ptr<const T> load() const {
        return std::atomic_load(&m_current);
    }

    void store(T value) {
        std::lock_guard lock(m_write_mutex);
        std::atomic_store(&m_current, std::make_shared<const T>(std::move(value)));
    }

    template<typename Fn>
    void update(Fn &&fn) {
        std::lock_guard lock(m_write_mutex);
        auto old = std::atomic_load(&m_current);
        std::atomic_store(&m_current, std::make_shared<const T>(fn(*old)));
    }
};

/* Copyright 2009-2018 Pierre Ossman for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifndef __RFB_CONGESTION_H__
#define __RFB_CONGESTION_H__

#include <array>
#include <cassert>

namespace rfb {
    // A fixed-capacity ring buffer: push_back()/pop_front() reuse the same
    // N array slots via wrapping indices, giving O(1) operations with no
    // allocation and no shifting. It is a strict bounded FIFO, not an
    // overwriting one — callers must not push_back() while full (check
    // size() < capacity() first); doing so is a caller bug and is only
    // asserted against, not handled, since a silent overwrite would mean
    // an unrelated older entry (e.g. one a caller elsewhere still expects
    // to pop_front()) is gone without anyone being told.
    template<typename T, size_t N>
    class CircularBuffer {
        std::array<T, N> buffer{};

        size_t head{};
        size_t tail{};
        size_t count{};

    public:
        CircularBuffer() = default;

        CircularBuffer(const CircularBuffer &) = delete;

        CircularBuffer &operator=(const CircularBuffer &) = delete;

        CircularBuffer(CircularBuffer &&) = delete;

        CircularBuffer &operator=(CircularBuffer &&) = delete;

        [[nodiscard]] bool empty() const { return count == 0; }

        [[nodiscard]] size_t size() const { return count; }
        static size_t capacity() { return N; }

        void push_back(T value) {
            assert(count < N);
            buffer[tail] = value;
            tail = (tail + 1) % N;
            ++count;
        }

        T pop_front() {
            T value = buffer[head];
            ++head;
            head %= N;
            --count;
            return value;
        }

        T &front() { return buffer[head]; }
        [[nodiscard]] const T &front() const { return buffer[head]; }

        T &back() { return buffer[(tail + N - 1) % N]; }
        [[nodiscard]] const T &back() const { return buffer[(tail + N - 1) % N]; }

        template <typename BufferPtr>
        class basic_iterator {
            BufferPtr buf;
            size_t idx;
            size_t remaining;

        public:
            basic_iterator(BufferPtr _buf, const size_t _idx, const size_t _remaining)
                : buf(_buf), idx(_idx), remaining(_remaining) {
            }

            decltype(auto) operator*() { return (*buf)[idx]; }
            T *operator->() { return std::addressof(**this); }

            basic_iterator &operator++() {
                ++idx;
                idx %= N;
                --remaining;
                return *this;
            }

            bool operator==(const basic_iterator &o) const {
                return buf == o.buf && idx == o.idx && remaining == o.remaining;
            }
        };

        using iterator = basic_iterator<std::array<T, N> *>;
        using const_iterator = basic_iterator<const std::array<T, N> *>;

        iterator begin() { return {&buffer, head, count}; }
        iterator end() { return {&buffer, tail, 0}; }

        [[nodiscard]] const_iterator cbegin() const { return {&buffer, head, count}; }
        [[nodiscard]] const_iterator cend() const { return {&buffer, tail, 0}; }
    };

    class Congestion {
    public:
        constexpr static size_t MAX_VALUES = 120;

        Congestion();

        ~Congestion() = default;

        // updatePosition() registers the current stream position and can
        // and should be called often.
        void updatePosition(unsigned pos);

        [[nodiscard]] bool canSendPing() const { return pings.size() < pings.capacity(); }

        // sentPing() must be called when a marker is placed on the
        // outgoing stream, and gotPong() when the response for such a
        // marker is received. sentPing() returns false, without queuing
        // anything, if canSendPing() would be false; the caller must not
        // place the marker on the wire in that case, since a marker with
        // nothing queued to match it against would desync the next pong.
        bool sentPing();

        void gotPong();

        // isCongested() determines if the transport is currently congested
        // or if more data can be sent.
        bool isCongested() const;

        // getUncongestedETA() returns the number of milliseconds until the
        // transport is no longer congested. Returns 0 if there is no
        // congestion, and -1 if it is unknown when the transport will no
        // longer be congested.
        int getUncongestedETA();

        // getBandwidth() returns the current bandwidth estimation in bytes
        // per second.
        size_t getBandwidth() const;

        unsigned getPingTime() const;
        double getJitter() const;

        // debugTrace() writes the current congestion window, as well as the
        // congestion window of the underlying TCP layer, to the specified
        // file
        void debugTrace(const char *filename, int fd);

    protected:
        unsigned getExtraBuffer() const;

        unsigned getInFlight() const;

        void updateCongestion();

    private:
        unsigned lastPosition;
        unsigned extraBuffer;
        timeval lastUpdate{};
        timeval lastSent{};

        unsigned baseRTT;
        unsigned congWindow;
        bool inSlowStart;

        unsigned safeBaseRTT;

        struct RTTInfo {
            timeval tv;
            unsigned pos;
            unsigned extra;
            bool congested;
        };

        CircularBuffer<RTTInfo, MAX_VALUES> pings;

        RTTInfo lastPong{};
        timeval lastPongArrival{};

        int measurements;
        timeval lastAdjustment{};
        unsigned minRTT, minCongestedRTT;

        double rttvar{0.0};
        double srtt{0.0};
    };
}

#endif

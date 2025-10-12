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

namespace rfb {
    template<typename T, size_t N>
    class CircularBuffer {
        std::array<T, N> buffer;

        size_t head{};
        size_t tail{};

    public:
        CircularBuffer() = default;

        CircularBuffer(const CircularBuffer &) = delete;

        CircularBuffer &operator=(const CircularBuffer &) = delete;

        CircularBuffer(const CircularBuffer &&) = delete;

        CircularBuffer &operator=(const CircularBuffer &&) = delete;

        bool empty() const { return size() == 0; }

        size_t size() const { return tail >= head ? tail - head : N + tail - head; }
        static size_t capacity() { return N; }

        void push_back(T value) {
            buffer[tail] = value;
            ++tail;
            tail %= N;
        }

        T pop_front() {
            T value = buffer[head];
            ++head;
            head %= N;
            return value;
        }

        T &front() { return buffer[head]; }
        const T &front() const { return buffer[head]; }


        using iterator = std::array<T, N>::iterator;
        using const_iterator = std::array<T, N>::const_iterator;

        iterator begin() { return buffer.begin() + head; }
        iterator end() { return buffer.begin() + tail; }

        const_iterator cbegin() const { return buffer.cbegin() + head; }
        const_iterator cend() const { return buffer.cbegin() + tail; }
    };

    class Congestion {
    public:
        constexpr static size_t MAX_VALUES = 120;

        Congestion();

        ~Congestion() = default;

        // updatePosition() registers the current stream position and can
        // and should be called often.
        void updatePosition(unsigned pos);

        // sentPing() must be called when a marker is placed on the
        // outgoing stream. gotPong() must be called when the response for
        // such a marker is received.
        void sentPing();

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
    };
}

#endif

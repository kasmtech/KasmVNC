/* Copyright (C) 2026 Kasm Technologies Corp.  All Rights Reserved.
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
#ifndef __RFB_WEBRTCPOLICY_H__
#define __RFB_WEBRTCPOLICY_H__

// Kept free of libdatachannel and RFB state so the logic can be unit-tested
// (tests/webrtc/).

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rfb {
namespace webrtcpolicy {

enum class HandshakePhase : uint8_t
{
    Idle = 0,
    AwaitingSdpAnswer = 1,
    AwaitingIceConnect = 2
};

const char *phaseName(HandshakePhase p) noexcept;
const char *phaseReason(HandshakePhase p) noexcept;

std::vector<std::string> splitCodecList(const char *payload, size_t len);

// previousMs is 0 before the first retry.
constexpr int64_t BACKOFF_FIRST_MS = 5000;
constexpr int64_t BACKOFF_MAX_MS = 30000;
int64_t nextBackoffMs(int64_t previousMs) noexcept;

// Spreads a ceiling from nextBackoffMs() over [cap/2, cap] so screens and clients that fail together
// do not retry in lock-step. rand is any uniformly distributed value.
int64_t jitteredBackoffMs(int64_t capMs, uint64_t rand) noexcept;

// Gate for ClientCapabilities. Re-advertising is rate-limited, and an identical
// list is ignored while a WebRTC session is already set up, so a client cannot
// force the server to tear down and rebuild its transports in a loop.
enum class CapabilityDecision
{
    Accept,
    RateLimited,
    Duplicate
};

class CapabilityGate {
public:
    static constexpr int64_t MIN_INTERVAL_NS = 1'000'000'000LL;

    CapabilityDecision check(int64_t nowNs, const std::string &payload, bool sessionActive) const noexcept;
    void accepted(int64_t nowNs, const std::string &payload);

private:
    int64_t lastNs = 0;
    std::string lastPayload;
};

constexpr size_t MAX_REMOTE_CANDIDATES = 32;
constexpr size_t MAX_CANDIDATE_BYTES = 512;

enum class CandidateDecision
{
    Accept,
    TooLong,
    TooMany,
    AlreadyConnected
};

CandidateDecision checkRemoteCandidate(size_t acceptedSoFar, size_t len, bool iceConnected) noexcept;

class TrailingIdrAccumulator {
public:
    static constexpr int MAX_SCREENS = 8;
    static constexpr int64_t WINDOW_NS = 2'000'000'000LL;
    static constexpr int MIN_AREA_PERCENT = 5;

    bool addAndCheckThreshold(int64_t nowNs, unsigned screen, uint64_t changedPx, uint64_t screenAreaPx) noexcept;
    void reset() noexcept;

private:
    int64_t windowStartNs = 0;
    uint64_t areaPx[MAX_SCREENS] = {};
};

constexpr int64_t IMAGE_IDLE_MARKER_INTERVAL_NS = 1'000'000'000LL;
constexpr unsigned IMAGE_IDLE_MARKERS = 2;
bool imageIdleMarkerDue(int64_t nowNs, int64_t lastFrameNs, unsigned markersSent) noexcept;

class RefusalMemory {
public:
    static constexpr int64_t MIN_INTERVAL_NS = 1'000'000'000LL;

    bool shouldSuppress(const std::string &reason, int64_t nowNs);
    void clear() noexcept;

private:
    std::vector<std::pair<std::string, int64_t>> lastNs;
};

bool requestTooSoon(int64_t lastNs, int64_t nowNs, int64_t minIntervalNs) noexcept;

} // namespace webrtcpolicy
} // namespace rfb

#endif

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

#include <rfb/webrtc/WebRTCPolicy.h>

#include <algorithm>
#include <iterator>

namespace rfb {
namespace webrtcpolicy {

const char *phaseName(HandshakePhase p) noexcept
{
    switch (p) {
        case HandshakePhase::Idle:
            return "Idle";
        case HandshakePhase::AwaitingSdpAnswer:
            return "AwaitingSdpAnswer";
        case HandshakePhase::AwaitingIceConnect:
            return "AwaitingIceConnect";
    }
    return "Unknown";
}

const char *phaseReason(HandshakePhase p) noexcept
{
    switch (p) {
        case HandshakePhase::AwaitingSdpAnswer:
            return "offer-timeout";
        case HandshakePhase::AwaitingIceConnect:
            return "connect-timeout";
        default:
            return "handshake-timeout";
    }
}

std::vector<std::string> splitCodecList(const char *payload, size_t len)
{
    std::vector<std::string> out;
    const std::string list(payload, len);
    size_t pos = 0;
    while (pos < list.size()) {
        const size_t comma = list.find(',', pos);
        const std::string tok = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!tok.empty())
            out.push_back(tok);
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return out;
}

int64_t nextBackoffMs(int64_t previousMs) noexcept
{
    return previousMs == 0 ? BACKOFF_FIRST_MS : std::min(previousMs * 2, BACKOFF_MAX_MS);
}

int64_t jitteredBackoffMs(int64_t capMs, uint64_t rand) noexcept
{
    const int64_t lo = capMs / 2;
    return lo + static_cast<int64_t>(rand % static_cast<uint64_t>(capMs - lo + 1));
}

CapabilityDecision CapabilityGate::check(int64_t nowNs, const std::string &payload, bool sessionActive) const noexcept
{
    if (lastNs != 0 && nowNs - lastNs < MIN_INTERVAL_NS)
        return CapabilityDecision::RateLimited;
    if (sessionActive && payload == lastPayload)
        return CapabilityDecision::Duplicate;
    return CapabilityDecision::Accept;
}

void CapabilityGate::accepted(int64_t nowNs, const std::string &payload)
{
    lastNs = nowNs;
    lastPayload = payload;
}

CandidateDecision checkRemoteCandidate(size_t acceptedSoFar, size_t len, bool iceConnected) noexcept
{
    if (iceConnected)
        return CandidateDecision::AlreadyConnected;
    if (len > MAX_CANDIDATE_BYTES)
        return CandidateDecision::TooLong;
    if (acceptedSoFar >= MAX_REMOTE_CANDIDATES)
        return CandidateDecision::TooMany;
    return CandidateDecision::Accept;
}

bool TrailingIdrAccumulator::addAndCheckThreshold(int64_t nowNs, unsigned screen, uint64_t changedPx, uint64_t screenAreaPx) noexcept
{
    if (screen >= MAX_SCREENS || screenAreaPx == 0)
        return false;
    if (windowStartNs == 0 || nowNs - windowStartNs >= WINDOW_NS) {
        reset();
        windowStartNs = nowNs;
    }
    areaPx[screen] += changedPx;
    return areaPx[screen] * 100 >= screenAreaPx * MIN_AREA_PERCENT;
}

void TrailingIdrAccumulator::reset() noexcept
{
    windowStartNs = 0;
    std::fill(std::begin(areaPx), std::end(areaPx), 0);
}

bool imageIdleMarkerDue(int64_t nowNs, int64_t lastFrameNs, unsigned markersSent) noexcept
{
    if (lastFrameNs == 0 || markersSent >= IMAGE_IDLE_MARKERS)
        return false;
    return nowNs - lastFrameNs >= static_cast<int64_t>(markersSent + 1) * IMAGE_IDLE_MARKER_INTERVAL_NS;
}

bool RefusalMemory::shouldSuppress(const std::string &reason, int64_t nowNs)
{
    for (auto &entry: lastNs) {
        if (entry.first != reason)
            continue;
        if (nowNs - entry.second < MIN_INTERVAL_NS)
            return true;
        entry.second = nowNs;
        return false;
    }
    lastNs.emplace_back(reason, nowNs);
    return false;
}

void RefusalMemory::clear() noexcept
{
    lastNs.clear();
}

bool requestTooSoon(int64_t lastNs, int64_t nowNs, int64_t minIntervalNs) noexcept
{
    return lastNs != 0 && nowNs - lastNs < minIntervalNs;
}

} // namespace webrtcpolicy
} // namespace rfb

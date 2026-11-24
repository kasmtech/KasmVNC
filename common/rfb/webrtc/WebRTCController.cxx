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

#include <rfb/webrtc/WebRTCController.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <utility>

#include <network/iceip.h>
#include <rdr/Exception.h>

#include <rfb/ConnParams.h>
#include <rfb/encodings.h>
#include <rfb/LogWriter.h>
#include <rfb/Region.h>
#include <rfb/ScreenSet.h>
#include <rfb/SMsgWriter.h>
#include <rfb/ServerCore.h>
#include <rfb/util.h>
#include <rfb/Watermark.h>
#include <rfb/webrtc/webrtcTypes.h>

using namespace rfb;

static LogWriter vlog("WebRTCController");

static_assert(webrtcpolicy::TrailingIdrAccumulator::MAX_SCREENS >= MAX_WEBRTC_SCREENS);

static uint64_t regionArea(const Region &region)
{
    std::vector<Rect> rects;
    region.get_rects(&rects);
    uint64_t area = 0;
    for (const Rect &r: rects)
        area += static_cast<uint64_t>(r.area());
    return area;
}

WebRTCController::WebRTCController(WebRTCHost &connection) :
    host(connection),
    trailingIdrTimer(this),
    webrtcMaintTimer(this)
{
}

WebRTCController::~WebRTCController() = default;

void WebRTCController::writeFallback(rdr::U8 screenId, const char *reason)
{
    host.msgWriter()->writeWebRTCFallback(screenId, reason, strlen(reason));
}

void WebRTCController::shutdown()
{
    resetPendingMedia();
    resetMedia();
    resetImageChannel();
}

void WebRTCController::revokeAccess()
{
    // The deferred retry runs from the maintenance tick, past the signal path's view-rights check.
    imageRequestDeferred = false;
    disableImageChannel("access-revoked", true);
    if (!webRTCRequested && webRTCScreens.empty())
        return;
    if (host.msgWriter())
        writeFallback(WEBRTC_SESSION_SCREEN, "access-revoked");
    abandonSession();
}

void WebRTCController::tickBeforeUpdate()
{
    syncCallbackState();
    tickImageChannel();
    if ((!webRTCScreens.empty() || !webRTCFailedScreens.empty() || imageTransport || imageRequestDeferred) &&
        !webrtcMaintTimer.isStarted())
        webrtcMaintTimer.start(WEBRTC_MAINT_TICK_MS);

    // Everything below works against deadlines of a second or more, so the maintenance timer's cadence is enough.
    if (!std::exchange(maintDue, false))
        return;
    host.imageStream()->trimIfIdle();
    tickReoffer();
    tickPendingRemoval();
    tickHandshakeTimeout();

    uint8_t iceFailed = 0;
    for (auto &kv: webRTCScreens) {
        WebRTCScreenSlot &slot = kv.second;
        const int64_t failedSinceNs = slot.live.cb ? slot.live.cb->failedSinceNs.load(std::memory_order_acquire) : 0;
        if (slot.live.transport && iceFailedFor(failedSinceNs, monotonicNowNs(), ICE_FAILURE_GRACE_NS))
            iceFailed |= 1u << kv.first;
    }
    if (iceFailed && host.connParams().supportsWebRTCMedia) {
        vlog.error("Client %s WebRTC ICE failed past its grace period — coordinated "
                   "fallback to WebSocket video (all screens), will re-offer",
            host.peerAddress());
        coordinatedFallback("ice-timeout");
    }
}

void WebRTCController::onFramebufferUpdated(const Region &changed, const Region &copied)
{
    if (!host.connParams().supportsWebRTCMedia || inTrailingIdrTick || webRTCScreens.empty())
        return;

    const Region touched = changed.union_(copied);
    const int64_t nowNs = monotonicNowNs();
    unsigned i = 0;
    for (const auto &s: host.layout().screens) {
        if (i >= MAX_WEBRTC_SCREENS)
            break;
        const unsigned bit = 1u << i;
        if (!(trailingIdrScreens & bit)) {
            const uint64_t changedPx = regionArea(touched.intersect(Region(s.dimensions)));
            if (changedPx && trailingIdrArea.addAndCheckThreshold(nowNs, i, changedPx, static_cast<uint64_t>(s.dimensions.area())))
                trailingIdrScreens |= bit;
        }
        ++i;
    }
    if (trailingIdrScreens)
        trailingIdrTimer.start(TRAILING_IDR_MS);
}

void WebRTCController::addImageFullFrameIfDue(Region &changed)
{
    if (!Server::udpFullFrameFrequency || !host.connParams().supportsImageChannel)
        return;
    if (imageFramesSinceFull >= (unsigned) Server::udpFullFrameFrequency) {
        imageFramesSinceFull = 0;
        changed.assign_union(Region(host.fullFrameRect()));
    }
}

void WebRTCController::noteImageFrameSent()
{
    if (!host.connParams().supportsImageChannel)
        return;
    lastImageFrameNs = monotonicNowNs();
    imageIdleMarkersSent = 0;
    if (Server::udpFullFrameFrequency)
        imageFramesSinceFull++;
}

void WebRTCController::tickImageIdleMarker()
{
    if (!host.connParams().supportsImageChannel)
        return;
    if (!webrtcpolicy::imageIdleMarkerDue(monotonicNowNs(), lastImageFrameNs, imageIdleMarkersSent))
        return;
    imageIdleMarkersSent++;

    network::ImageChannelStream *stream = host.imageStream();
    for (int i = 0; i < 4; i++)
        stream->writeU16(0);
    stream->writeS32(pseudoEncodingImageChannelMarker);
    stream->flush();
}

bool WebRTCController::handleTimeout(Timer *t)
{
  try {
    if (t == &trailingIdrTimer)
        tickTrailingVideoIdr();
    else if (t == &webrtcMaintTimer)
        tickMaintenance();
  } catch (rdr::Exception& e) {
    host.disconnect(e.str());
  }

  return false;
}

bool WebRTCController::allScreensRoutedOverRtp() const {
    if (!host.connParams().supportsWebRTCMedia || webRTCScreens.empty())
        return false;
    for (const auto &kv: webRTCScreens) {
        const auto &slot = kv.second;
        if (!slot.live.transport || !slot.live.transport->isReady())
            return false;
    }
    return true;
}

uint8_t WebRTCController::activeScreenMask() const {
    const unsigned n = std::min<unsigned>(static_cast<unsigned>(host.layout().num_screens()),
        static_cast<unsigned>(MAX_WEBRTC_SCREENS));
    return static_cast<uint8_t>((1u << n) - 1);
}

void WebRTCController::forceScreenKeyframe(rdr::U8 screenId) {
    unsigned i = 0;
    for (const auto &s: host.layout().screens) {
        if (i++ == static_cast<unsigned>(screenId)) {
            host.markChanged(Region(s.dimensions));
            break;
        }
    }
    host.forceScreenKeyframe(screenId);
}

void WebRTCController::moveScreenToWebSocket(rdr::U8 screenId, const char *reason) {
    if (reason)
        writeFallback(screenId, reason);
    resetScreen(screenId);
    markScreenFailed(screenId);
    forceScreenKeyframe(screenId);
}

void WebRTCController::markScreenFailed(rdr::U8 screenId) {
    const int64_t nowNs = monotonicNowNs();
    advanceBackoff(webRTCFailedScreens[screenId], nowNs);
}

void WebRTCController::advanceBackoff(WebRTCFailback &fb, int64_t nowNs) {
    fb.capMs = webrtcpolicy::nextBackoffMs(fb.capMs);
    fb.backoffMs = webrtcpolicy::jitteredBackoffMs(fb.capMs, backoffRng());
    fb.nextReofferNs = nowNs + fb.backoffMs * 1000000LL;
}

void WebRTCController::tickReoffer() {
    if (webRTCFailedScreens.empty())
        return;
    if (host.connState() != SConnection::RFBSTATE_NORMAL)
        return;

    const int64_t nowNs = monotonicNowNs();
    const uint8_t active = activeScreenMask();

    const auto inFlight = [this](rdr::U8 id) {
        const auto it = webRTCScreens.find(id);
        return it != webRTCScreens.end() && it->second.live.transport;
    };
    bool exhausted = false;
    for (const auto &kv: webRTCFailedScreens) {
        if (inFlight(kv.first))
            continue;
        if (kv.second.attempts >= MAX_WEBRTC_REOFFER_ATTEMPTS) {
            exhausted = true;
            break;
        }
    }
    if (exhausted) {
        vlog.error("Client %s WebRTC re-offer retries exhausted (%d attempts) — "
                   "falling back to image mode",
            host.peerAddress(),
            MAX_WEBRTC_REOFFER_ATTEMPTS);
        writeFallback(WEBRTC_SESSION_SCREEN, "retries-exhausted");
        resetPendingMedia();
        fallbackToImageMode(true);
        resetMedia();
        webRTCFailedScreens.clear();
        return;
    }

    std::vector<rdr::U8> toReoffer;
    for (const auto &kv: webRTCFailedScreens)
        if (((active >> kv.first) & 1) && !inFlight(kv.first) && nowNs >= kv.second.nextReofferNs)
            toReoffer.push_back(kv.first);

    for (rdr::U8 id: toReoffer) {
        WebRTCFailback &fb = webRTCFailedScreens[id];
        fb.attempts += 1;
        advanceBackoff(fb, nowNs);
        vlog.info("Re-offering WebRTC screen %u after recovery backoff (%lldms, "
                  "attempt %d/%d)",
            id,
            (long long) fb.backoffMs,
            fb.attempts,
            MAX_WEBRTC_REOFFER_ATTEMPTS);
        offerScreen(id, false);
    }
}

void WebRTCController::tickTrailingVideoIdr() {
    const rdr::U8 touched = trailingIdrScreens;
    trailingIdrScreens = 0;
    trailingIdrArea.reset();
    if (host.connState() != SConnection::RFBSTATE_NORMAL)
        return;
    if (!host.connParams().supportsWebRTCMedia)
        return;
    if (webRTCScreens.empty() || !touched)
        return;

    inTrailingIdrTick = true;
    for (const auto &kv: webRTCScreens) {
        if (kv.second.live.transport && kv.first < MAX_WEBRTC_SCREENS && (touched & (1u << kv.first)))
            forceScreenKeyframe(kv.first);
    }
    try {
        host.sendFramebufferUpdate();
    } catch (...) {
        inTrailingIdrTick = false;
        throw;
    }
    inTrailingIdrTick = false;
}

void WebRTCController::tickMaintenance() {
    if (host.connState() != SConnection::RFBSTATE_NORMAL) {
        imageRequestDeferred = false;
        return;
    }

    tickWatermarkResend();
    resendIceServersIfChanged();
    tickImageIdleMarker();
    if (imageRequestDeferred &&
        !webrtcpolicy::requestTooSoon(lastImageRequestNs, monotonicNowNs(), WEBRTC_REQUEST_OFFER_MIN_INTERVAL_NS)) {
        imageRequestDeferred = false;
        handleImageRequest();
    }
    maintDue = true;
    host.sendFramebufferUpdate();

    if (!webRTCScreens.empty() || !webRTCFailedScreens.empty() || imageTransport || imageRequestDeferred)
        webrtcMaintTimer.start(WEBRTC_MAINT_TICK_MS);
}

// The image channel can lose the watermark without anyone noticing, so repeat it.
// A one pixel change makes an update that carries it.
void WebRTCController::tickWatermarkResend() {
    if (!watermarkData || !host.connParams().supportsImageChannel)
        return;

    const int64_t nowNs = monotonicNowNs();
    if (nowNs < nextWatermarkResendNs)
        return;
    nextWatermarkResendNs = nowNs + WATERMARK_RESEND_NS;

    host.requestWatermark();
    host.markChanged(Region(Rect(0, 0, 1, 1)));
}

void WebRTCController::tickHandshakeTimeout() {
    if (host.connState() != SConnection::RFBSTATE_NORMAL)
        return;
    if (webRTCScreens.empty())
        return;

    const int64_t now = monotonicNowNs();

    struct Stuck {
        rdr::U8 id;
        WebRTCHandshakePhase phase;
    };
    std::vector<Stuck> stuckLive;
    std::vector<Stuck> stuckPending;

    for (auto &kv: webRTCScreens) {
        WebRTCScreenSlot &slot = kv.second;

        if (slot.live.deadlineNs != 0 && now > slot.live.deadlineNs && slot.live.transport) {
            if (slot.live.transport->isReady()) {
                slot.live.deadlineNs = 0;
                slot.live.phase = WebRTCHandshakePhase::Idle;
            } else {
                stuckLive.push_back({kv.first, slot.live.phase});
            }
        }

        if (slot.pending.deadlineNs != 0 && now > slot.pending.deadlineNs && slot.pending.transport) {
            if (slot.pending.transport->isReady()) {
                slot.pending.deadlineNs = 0;
                slot.pending.phase = WebRTCHandshakePhase::Idle;
            } else {
                stuckPending.push_back({kv.first, slot.pending.phase});
            }
        }
    }

    for (const Stuck &s: stuckPending) {
        vlog.error("Pending WebRTC transport (screen %u) stuck in %s after "
                   "handshake deadline; abandoning renegotiation",
            s.id,
            webrtcpolicy::phaseName(s.phase));
        const char *reason = "renegotiate-timeout";
        writeFallback(s.id, reason);
        resetPendingScreen(s.id);
    }

    if (stuckLive.empty())
        return;

    const char *reason = webrtcpolicy::phaseReason(stuckLive.front().phase);
    vlog.error("Client %s WebRTC handshake stuck on %zu screen(s) (%s) — "
               "coordinated fallback to WebSocket video, will re-offer",
        host.peerAddress(),
        stuckLive.size(),
        reason);
    coordinatedFallback(reason);
}

void WebRTCController::coordinatedFallback(const char *reason) {
    writeFallback(WEBRTC_SESSION_SCREEN, reason);
    resetPendingMedia();
    std::vector<rdr::U8> all;
    for (const auto &kv: webRTCScreens)
        all.push_back(kv.first);
    for (rdr::U8 id: all)
        moveScreenToWebSocket(id, nullptr);
}

void WebRTCController::syncCallbackState() {
    for (auto &kv: webRTCScreens) {
        const rdr::U8 screenId = kv.first;
        WebRTCScreenSlot &slot = kv.second;

        auto drainCandidates = [this, screenId](const std::shared_ptr<WebRTCCallbackState> &cb) {
            if (!cb)
                return;
            std::vector<std::string> out;
            {
                std::lock_guard<std::mutex> lk(cb->pendingCandidatesMu);
                out.swap(cb->pendingCandidates);
            }
            for (const auto &payload: out) {
                host.msgWriter()->writeWebRTCIceCandidate(screenId, payload.data(), payload.size());
            }
        };
        drainCandidates(slot.live.cb);
        drainCandidates(slot.pending.cb);

        if (slot.live.cb && slot.live.cb->pendingClientRefresh.exchange(false, std::memory_order_acq_rel)) {
            forceScreenKeyframe(screenId);
        }

        if (slot.live.cb) {
            const bool routed = slot.live.transport && slot.live.transport->isReady();
            host.setScreenMaxBitrate(screenId, routed ? Server::webRTCMaxBitrate : 0);
        }

        if (slot.live.cb && slot.live.transport && slot.live.transport->isReady() &&
            slot.live.cb->handshakeCompleteNs.load(std::memory_order_acquire) != 0) {
            slot.live.deadlineNs = 0;
            slot.live.phase = WebRTCHandshakePhase::Idle;
            webRTCFailedScreens.erase(screenId);
        }

        if (slot.pending.cb && slot.pending.transport) {
            const bool pendingReady = slot.pending.transport->isReady();
            if (pendingReady && slot.pending.cb->readyToPromote.exchange(false, std::memory_order_acq_rel)) {
                slot.pending.deadlineNs = 0;
                slot.pending.phase = WebRTCHandshakePhase::Idle;
                slot.live.deadlineNs = 0;
                slot.live.phase = WebRTCHandshakePhase::Idle;
                promoteMedia(screenId);
                continue;
            }
            if (pendingReady && slot.pending.deadlineNs != 0 &&
                slot.pending.cb->handshakeCompleteNs.load(std::memory_order_acquire) != 0) {
                slot.pending.deadlineNs = 0;
                slot.pending.phase = WebRTCHandshakePhase::Idle;
            }
            const int64_t failedSinceNs = slot.pending.cb->failedSinceNs.load(std::memory_order_acquire);
            if (iceFailedFor(failedSinceNs, monotonicNowNs(), ICE_FAILURE_GRACE_NS)) {
                vlog.info("Pending WebRTC transport (screen %u) ICE failed past its "
                          "grace period; abandoning renegotiation, live transport stays up",
                    screenId);
                const char *reason = "renegotiate-ice-failed";
                writeFallback(screenId, reason);
                resetPendingScreen(screenId);
            }
        }
    }

    const rdr::U8 mismatch = host.takeCodecMismatch();
    for (rdr::U8 id = 0; mismatch && id < MAX_WEBRTC_SCREENS; ++id) {
        if (!(mismatch & (1u << id)))
            continue;
        auto it = webRTCScreens.find(id);
        if (it == webRTCScreens.end() || !it->second.live.transport || it->second.pending.transport)
            continue;
        vlog.error("WebRTC screen %u: encoder codec no longer matches the "
                   "negotiated %s; falling this screen back to WebSocket video",
            id,
            rfb::kasmVideoCodecName(it->second.live.transport->negotiatedCodec()));
        moveScreenToWebSocket(id, "codec-mismatch");
    }

    // The server stopped encoding video, so a live WebRTC session would show a frozen frame over the image
    // rects. The reason is deliberately not one the client retries: it must treat the session as ended,
    // since the server has dropped it (webRTCRequested=false) and will not re-offer on its own.
    if (host.takeEncoderFailed() && host.connParams().supportsWebRTCMedia) {
        vlog.error("Client %s video encoding failed; dropping WebRTC media for image mode", host.peerAddress());
        writeFallback(WEBRTC_SESSION_SCREEN, "encoder-failed");
        abandonSession();
    }
}

void WebRTCController::resetPendingScreen(rdr::U8 screenId) {
    auto it = webRTCScreens.find(screenId);
    if (it == webRTCScreens.end())
        return;
    WebRTCScreenSlot &slot = it->second;
    if (slot.pending.cb)
        slot.pending.cb->active.store(false, std::memory_order_release);
    if (slot.pending.transport) {
        slot.pending.transport->onKeyframeRequest({});
        slot.pending.transport->onIceStateChanged({});
        slot.pending.transport->onTrackOpen({});
        slot.pending.transport->onLocalCandidate({});
    }
    slot.pending.transport.reset();
    slot.pending.cb.reset();

    bool anyPending = false;
    for (const auto &kv: webRTCScreens)
        if (kv.second.pending.transport) {
            anyPending = true;
            break;
        }
    if (!anyPending)
        clearDeferredEncoderConfig();
}

void WebRTCController::resetPendingMedia() {
    for (auto &kv: webRTCScreens)
        resetPendingScreen(kv.first);
    clearDeferredEncoderConfig();
}

void WebRTCController::beginDeferredEncoderConfig() {
    // The live encoder keeps running until the new transports are promoted.
    host.connParams().pendingEncoderConfig = host.connParams().encoder_config;
    host.connParams().encoder_config = liveWebRTCEncoderConfig;
    host.connParams().deferEncoderConfig = true;
}

void WebRTCController::commitDeferredEncoderConfig(KasmVideoEncoders::EncoderConfig cfg) {
    host.connParams().encoder_config = cfg;
    clearDeferredEncoderConfig();
    liveWebRTCEncoderConfig = host.connParams().encoder_config;
}

void WebRTCController::clearDeferredEncoderConfig() {
    host.connParams().deferEncoderConfig = false;
    host.connParams().pendingEncoderConfig = KasmVideoEncoders::EncoderConfig{KasmVideoEncoders::Encoder::unavailable};
}

void WebRTCController::promoteMedia(rdr::U8 screenId) {
    auto it = webRTCScreens.find(screenId);
    if (it == webRTCScreens.end() || !it->second.pending.transport)
        return;
    WebRTCScreenSlot &slot = it->second;

    vlog.info("Promoting pending WebRTC transport (screen %u, codec=%s)",
        screenId,
        rfb::kasmVideoCodecName(slot.pending.transport->negotiatedCodec()));
    clearMediaCallbacks(screenId);
    host.attachWrtcTransport(screenId, nullptr);
    slot.live = std::move(slot.pending);
    slot.pending = WebRTCLeg{};
    host.attachWrtcTransport(screenId, slot.live.transport.get());

    if (slot.live.transport)
        host.setScreenMaxBitrate(screenId, Server::webRTCMaxBitrate);

    if (host.connParams().deferEncoderConfig && host.connParams().pendingEncoderConfig.encoder != KasmVideoEncoders::Encoder::unavailable)
        commitDeferredEncoderConfig(host.connParams().pendingEncoderConfig);

    forceScreenKeyframe(screenId);
}

void WebRTCController::clearMediaCallbacks(rdr::U8 screenId) {
    auto it = webRTCScreens.find(screenId);
    if (it == webRTCScreens.end())
        return;
    WebRTCScreenSlot &slot = it->second;
    if (slot.live.cb)
        slot.live.cb->active.store(false, std::memory_order_release);
    if (!slot.live.transport)
        return;
    slot.live.transport->onKeyframeRequest({});
    slot.live.transport->onIceStateChanged({});
    slot.live.transport->onTrackOpen({});
    slot.live.transport->onLocalCandidate({});
    host.setScreenMaxBitrate(screenId, 0);
}

void WebRTCController::resetScreen(rdr::U8 screenId) {
    resetPendingScreen(screenId);
    clearMediaCallbacks(screenId);
    host.attachWrtcTransport(screenId, nullptr);
    auto it = webRTCScreens.find(screenId);
    if (it != webRTCScreens.end()) {
        it->second.live.transport.reset();
        it->second.live.cb.reset();
        webRTCScreens.erase(it);
    }
}

void WebRTCController::resetMedia() {
    std::vector<rdr::U8> ids;
    ids.reserve(webRTCScreens.size());
    for (const auto &kv: webRTCScreens)
        ids.push_back(kv.first);
    for (rdr::U8 id: ids)
        resetScreen(id);
    webRTCScreens.clear();
    webRTCFailedScreens.clear();
    webRTCPendingRemoval.clear();
    webRTCRequested = false;
    sentIceGeneration.reset();
    webRTCOfferCodecs.clear();
    liveWebRTCEncoderConfig = KasmVideoEncoders::EncoderConfig{KasmVideoEncoders::Encoder::unavailable};
}

void WebRTCController::handleSignal(rdr::U8 kind, rdr::U8 screenId, const char *payload, size_t len) {
    const auto k = static_cast<WebRTCSignalKind>(kind);

    if (screenId == WEBRTC_IMAGE_SCREEN) {
        handleImageSignal(k, payload, len);
        return;
    }

    if (k != WebRTCSignalKind::ClientCapabilities && !webRTCRequested) {
        vlog.error("WebRTC signal kind=%u arrived before ClientCapabilities; dropping", kind);
        return;
    }

    switch (k) {
        case WebRTCSignalKind::Fallback:
            handleClientFallback(screenId, payload, len);
            return;
        case WebRTCSignalKind::SdpAnswer:
            handleSdpAnswer(screenId, payload, len);
            return;
        case WebRTCSignalKind::IceCandidate:
            handleIceCandidate(screenId, payload, len);
            return;
        case WebRTCSignalKind::ClientCapabilities:
            handleClientCapabilities(payload, len);
            return;
        case WebRTCSignalKind::RequestOffer:
            handleRequestOffer(screenId);
            return;
        case WebRTCSignalKind::ImageRequest:
            vlog.error("ImageRequest for non-image screen %u; dropping", screenId);
            return;
        case WebRTCSignalKind::SdpOffer:
        case WebRTCSignalKind::IceServers:
        case WebRTCSignalKind::Close:
            vlog.error("Client sent server-only WebRTC signal kind %d", kind);
            return;
    }
    vlog.error("Unknown WebRTC signal kind %d", kind);
}

void WebRTCController::sendIceServers(const turn::Snapshot &snapshot) {
    const std::string config = turn::iceConfigJson(iceStunUrls(), &snapshot.credentials, snapshot.forceRelay);
    host.msgWriter()->writeWebRTCIceServers(config.data(), config.size());
    sentIceGeneration = snapshot.generation;
}

// Live peers keep their allocations; fresh credentials matter for the next offer or ICE restart.
void WebRTCController::resendIceServersIfChanged() {
    if (!sentIceGeneration)
        return;
    const auto snapshot = turn::CredentialSource::instance().snapshot();
    if (snapshot.generation != *sentIceGeneration)
        sendIceServers(snapshot);
}

network::WebRTCMediaTransport::Config WebRTCController::makeConfig(rdr::U8 screenId) const {
    network::WebRTCMediaTransport::Config c;
    c.screenId = screenId;
    c.stunUrls = iceStunUrls();
    c.turn = turn::CredentialSource::instance().snapshot().credentials;
    c.publicIP = static_cast<const char *>(Server::publicIP);
    const int portBegin = Server::webRTCPortRangeBegin;
    const int portEnd = Server::webRTCPortRangeEnd;
    if (portBegin > 0 && portEnd >= portBegin) {
        c.portRangeBegin = static_cast<uint16_t>(portBegin);
        c.portRangeEnd = static_cast<uint16_t>(portEnd);
    }
    c.offerCodecs = webRTCOfferCodecs;
    return c;
}

void WebRTCController::wireCallbacks(
    network::WebRTCMediaTransport *t, rdr::U8 screenId, std::shared_ptr<WebRTCCallbackState> cbState, bool isPending) {
    (void) screenId;
    t->onKeyframeRequest([cbState](uint8_t) {
        if (!cbState->active.load(std::memory_order_acquire))
            return;
        cbState->pendingClientRefresh.store(true, std::memory_order_release);
    });
    t->onIceStateChanged([cbState, isPending](network::WebRTCMediaTransport::IceState s) {
        if (!cbState->active.load(std::memory_order_acquire))
            return;
        using IS = network::WebRTCMediaTransport::IceState;
        if (s == IS::Failed || s == IS::Disconnected || s == IS::Closed) {
            int64_t zero = 0;
            cbState->failedSinceNs.compare_exchange_strong(zero, monotonicNowNs(), std::memory_order_acq_rel);
        } else if (s == IS::Connected || s == IS::Completed) {
            cbState->failedSinceNs.store(0, std::memory_order_release);
            if (isPending && (s == IS::Connected || s == IS::Completed))
                cbState->readyToPromote.store(true, std::memory_order_release);
            if (s == IS::Connected || s == IS::Completed) {
                int64_t zero2 = 0;
                cbState->handshakeCompleteNs.compare_exchange_strong(zero2, monotonicNowNs(), std::memory_order_acq_rel);
            }
        }
    });
    t->onTrackOpen([cbState](uint8_t) {
        if (!cbState->active.load(std::memory_order_acquire))
            return;
        cbState->pendingClientRefresh.store(true, std::memory_order_release);
    });
    t->onLocalCandidate([cbState](const std::string &candidate, const std::string &mid) {
        if (!cbState->active.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lk(cbState->pendingCandidatesMu);
        cbState->pendingCandidates.push_back(candidate + "|" + mid);
    });
}

bool WebRTCController::offerScreen(rdr::U8 screenId, bool pending) {
    if (screenId >= MAX_WEBRTC_SCREENS) {
        ++webRTCRejectedScreenIds;
        vlog.error("offerScreen(screen %u): exceeds max %u WebRTC screens "
                   "(rejected so far=%llu)",
            screenId,
            static_cast<unsigned>(MAX_WEBRTC_SCREENS),
            (unsigned long long) webRTCRejectedScreenIds);
        if (!pending)
            writeFallback(screenId, "screen-out-of-range");
        return false;
    }
    if (webRTCOfferCodecs.empty()) {
        vlog.error("offerScreen(screen %u): no offer codecs", screenId);
        return false;
    }
    auto cfg = makeConfig(screenId);
    std::unique_ptr<network::WebRTCMediaTransport> transport;
    try {
        transport = std::make_unique<network::WebRTCMediaTransport>(cfg);
    } catch (const std::exception &e) {
        vlog.error("WebRTCMediaTransport ctor threw for screen %u: %s", screenId, e.what());
        if (!pending)
            writeFallback(screenId, "ctor-failed");
        return false;
    }
    auto cbState = std::make_shared<WebRTCCallbackState>();
    cbState->active.store(true, std::memory_order_release);
    wireCallbacks(transport.get(), screenId, cbState, pending);

    std::string sdp = transport->createOffer();
    if (sdp.empty()) {
        vlog.error("createOffer returned empty SDP for screen %u", screenId);
        cbState->active.store(false, std::memory_order_release);
        if (!pending)
            writeFallback(screenId, "offer-failed");
        return false;
    }

    WebRTCScreenSlot &slot = webRTCScreens[screenId];
    if (pending) {
        slot.pending.transport = std::move(transport);
        slot.pending.cb = std::move(cbState);
    } else {
        slot.live.transport = std::move(transport);
        slot.live.cb = std::move(cbState);
    }
    host.msgWriter()->writeWebRTCOffer(screenId, sdp.data(), sdp.size());
    vlog.info("Sent WebRTC %s offer for screen %u", pending ? "pending" : "live", screenId);

    const int64_t offerTimeoutMs = static_cast<int64_t>((int) Server::webrtcOfferToAnswerTimeoutMs);
    if (offerTimeoutMs > 0) {
        const int64_t deadline = monotonicNowNs() + offerTimeoutMs * 1000000LL;
        if (pending) {
            slot.pending.deadlineNs = deadline;
            slot.pending.phase = WebRTCHandshakePhase::AwaitingSdpAnswer;
        } else {
            slot.live.deadlineNs = deadline;
            slot.live.phase = WebRTCHandshakePhase::AwaitingSdpAnswer;
        }
    }
    return true;
}

bool WebRTCController::ensureScreen(rdr::U8 screenId) {
    if (webRTCFailedScreens.count(screenId))
        return true;
    auto it = webRTCScreens.find(screenId);
    if (it != webRTCScreens.end() && (it->second.live.transport || it->second.pending.transport))
        return true;
    if (offerScreen(screenId, false))
        return true;
    if (screenId < MAX_WEBRTC_SCREENS)
        markScreenFailed(screenId);
    return false;
}

void WebRTCController::removeScreen(rdr::U8 screenId) {
    if (webRTCScreens.find(screenId) == webRTCScreens.end())
        return;
    vlog.info("Removing WebRTC transport for screen %u (left layout)", screenId);
    host.msgWriter()->writeWebRTCClose(screenId, "screen-removed", strlen("screen-removed"));
    webRTCFailedScreens.erase(screenId);
    resetScreen(screenId);
}

void WebRTCController::reconcileScreens() {
    if (!webRTCRequested)
        return;
    if (host.connState() != SConnection::RFBSTATE_NORMAL)
        return;

    const uint8_t active = activeScreenMask();
    const int64_t nowNs = monotonicNowNs();

    for (const auto &kv: webRTCScreens)
        if (!((active >> kv.first) & 1) && !webRTCPendingRemoval.count(kv.first))
            webRTCPendingRemoval[kv.first] = nowNs;

    for (rdr::U8 id = 0; id < MAX_WEBRTC_SCREENS; ++id) {
        if (!((active >> id) & 1))
            continue;
        webRTCPendingRemoval.erase(id);
        ensureScreen(id);
    }
}

void WebRTCController::tickPendingRemoval() {
    if (webRTCPendingRemoval.empty())
        return;
    const int64_t nowNs = monotonicNowNs();
    const int64_t debounceNs = std::chrono::duration_cast<std::chrono::nanoseconds>(WEBRTC_REMOVAL_DEBOUNCE).count();
    const int64_t maxDeferNs = std::chrono::duration_cast<std::chrono::nanoseconds>(WEBRTC_REMOVAL_MAX_DEFER).count();
    const uint8_t active = activeScreenMask();

    std::vector<rdr::U8> toRemove;
    for (auto it = webRTCPendingRemoval.begin(); it != webRTCPendingRemoval.end();) {
        const int64_t missingNs = nowNs - it->second;
        if ((active >> it->first) & 1) {
            it = webRTCPendingRemoval.erase(it);
        } else if (missingNs >= debounceNs) {
            auto slotIt = webRTCScreens.find(it->first);
            const bool connected = slotIt != webRTCScreens.end() && slotIt->second.live.transport &&
                                   slotIt->second.live.transport->isReady();
            if (connected && missingNs < maxDeferNs) {
                vlog.debug("deferring removal of screen %u — transport "
                           "still connected (likely transient layout glitch)",
                    it->first);
                ++it;
            } else {
                toRemove.push_back(it->first);
                it = webRTCPendingRemoval.erase(it);
            }
        } else {
            ++it;
        }
    }
    for (rdr::U8 id: toRemove)
        removeScreen(id);
}

void WebRTCController::handleClientFallback(rdr::U8 screenId, const char *payload, size_t len) {
    vlog.info("Client signalled WebRTC fallback (screen %u: %.*s)",
        screenId,
        (int) std::min(len, (size_t) 128),
        payload ? payload : "");
    const bool isRenegotiate =
        (len == strlen("renegotiate") && payload && std::memcmp(payload, "renegotiate", len) == 0);
    if (isRenegotiate)
        return;

    if (screenId != WEBRTC_SESSION_SCREEN) {
        if (screenId >= MAX_WEBRTC_SCREENS)
            return;
        moveScreenToWebSocket(screenId, nullptr);
        return;
    }

    resetPendingMedia();
    if (host.hasWrtcTransport() || host.connParams().supportsWebRTCMedia)
        fallbackToImageMode(false);
    resetMedia();
}

void WebRTCController::handleRequestOffer(rdr::U8 screenId) {
    if (screenId == WEBRTC_SESSION_SCREEN)
        return;
    if (!webRTCRequested)
        return;
    if (host.connState() != SConnection::RFBSTATE_NORMAL)
        return;

    if (screenId >= MAX_WEBRTC_SCREENS) {
        ++webRTCRejectedScreenIds;
        vlog.info("RequestOffer for screen %u ignored (exceeds max %u, "
                  "rejected so far=%llu)",
            screenId,
            static_cast<unsigned>(MAX_WEBRTC_SCREENS),
            (unsigned long long) webRTCRejectedScreenIds);
        return;
    }

    if (static_cast<unsigned>(screenId) >= static_cast<unsigned>(host.layout().num_screens())) {
        vlog.info("RequestOffer for screen %u ignored (not in layout)", screenId);
        return;
    }

    const int64_t nowNs = monotonicNowNs();
    int64_t &lastNs = webRTCLastRequestOfferNs[screenId];
    if (lastNs != 0 && nowNs - lastNs < WEBRTC_REQUEST_OFFER_MIN_INTERVAL_NS) {
        vlog.info("RequestOffer for screen %u rate-limited", screenId);
        return;
    }
    lastNs = nowNs;

    vlog.info("RequestOffer for screen %u — dropping any stale "
              "transport and re-offering",
        screenId);
    webRTCFailedScreens.erase(screenId);
    webRTCPendingRemoval.erase(screenId);
    resetScreen(screenId);
    if (!offerScreen(screenId, false))
        markScreenFailed(screenId);
}

void WebRTCController::handleSdpAnswer(rdr::U8 screenId, const char *payload, size_t len) {
    auto it = webRTCScreens.find(screenId);
    if (it == webRTCScreens.end()) {
        vlog.error("Got SDP answer for unknown screen %u", screenId);
        return;
    }
    WebRTCScreenSlot &slot = it->second;
    const bool liveAwaiting = slot.live.transport && slot.live.transport->awaitingAnswer();
    const bool isPending = static_cast<bool>(slot.pending.transport) && !liveAwaiting;
    auto *target = isPending ? slot.pending.transport.get() : slot.live.transport.get();
    if (!target) {
        vlog.error("Got SDP answer for screen %u with no pending offer", screenId);
        return;
    }
    target->handleAnswer(std::string(payload, len));
    auto codec = target->negotiatedCodec();
    if (codec == rfb::kasmVideoUnknownCodec) {
        if (isPending) {
            vlog.error("Pending SDP answer rejected for screen %u — no codec "
                       "overlap; keeping live transport",
                screenId);
            writeFallback(screenId, "renegotiate-rejected");
            resetPendingScreen(screenId);
            return;
        }
        vlog.error("SDP answer rejected for screen %u — no codec overlap; "
                   "this screen falls back to WebSocket video",
            screenId);
        host.msgWriter()->writeWebRTCClose(screenId, "codec-mismatch", strlen("codec-mismatch"));
        resetScreen(screenId);
        forceScreenKeyframe(screenId);
        return;
    }
    const int64_t connectTimeoutMs = static_cast<int64_t>((int) Server::webrtcAnswerToConnectTimeoutMs);
    WebRTCLeg &leg = isPending ? slot.pending : slot.live;
    if (connectTimeoutMs > 0) {
        leg.deadlineNs = monotonicNowNs() + connectTimeoutMs * 1000000LL;
        leg.phase = WebRTCHandshakePhase::AwaitingIceConnect;
    } else {
        leg.deadlineNs = 0;
        leg.phase = WebRTCHandshakePhase::Idle;
    }

    if (isPending) {
        vlog.info("Client %s screen %u pending WebRTC negotiated SDP "
                  "(codec=%s); awaiting ICE",
            host.peerAddress(),
            screenId,
            rfb::kasmVideoCodecName(codec));
    } else {
        host.connParams().supportsWebRTCMedia = true;
        host.attachWrtcTransport(screenId, slot.live.transport.get());
        liveWebRTCEncoderConfig = host.connParams().encoder_config;
        vlog.info("Client %s screen %u negotiated WebRTC media (codec=%s)",
            host.peerAddress(),
            screenId,
            rfb::kasmVideoCodecName(codec));
    }
}

void WebRTCController::handleClientCapabilities(const char *payload, size_t len) {
    const int64_t nowNs = monotonicNowNs();

    // The image DataChannel and WebRTC video are exclusive: video frames are not encoded while
    // image rects are in use, so negotiating video here would leave a dead screen.
    if (imageTransport || host.connParams().supportsImageChannel) {
        if (!refusals.shouldSuppress("image-active", nowNs))
            vlog.info("Client %s advertised WebRTC video while the image DataChannel is active; refusing",
                host.peerAddress());
        writeFallback(WEBRTC_SESSION_SCREEN, "image-active");
        return;
    }

    std::vector<rfb::KasmVideoCodecId> requested;
    for (const std::string &tok: webrtcpolicy::splitCodecList(payload, len)) {
        const auto code = rfb::kasmVideoCodecFromToken(tok);
        if (code != rfb::kasmVideoUnknownCodec)
            requested.push_back(code);
    }
    if (requested.empty())
        requested.push_back(rfb::kasmVideoH264);

    std::set<rfb::KasmVideoCodecId> hwCodecs;
    for (const auto &ec: host.connParams().available_encoders) {
        if (ec.encoder == KasmVideoEncoders::Encoder::unavailable)
            continue;
        switch (KasmVideoEncoders::to_msg_id(ec.encoder)) {
            case kasmVideoH264:
                hwCodecs.insert(rfb::kasmVideoH264);
                break;
            case kasmVideoH265:
                hwCodecs.insert(rfb::kasmVideoH265);
                break;
            case kasmVideoAV1:
                hwCodecs.insert(rfb::kasmVideoAV1);
                break;
            default:
                break;
        }
    }
    if (hwCodecs.empty()) {
        if (!refusals.shouldSuppress("no-server-codec", nowNs)) {
            vlog.error("HW intersection: cp.available_encoders is empty — "
                       "EncoderProbe produced no usable codec. WebRTC media "
                       "cannot be offered. Falling back to image mode.");
            abandonSession();
        }
        writeFallback(WEBRTC_SESSION_SCREEN, "no-server-codec");
        return;
    }
    std::vector<rfb::KasmVideoCodecId> intersected;
    for (auto code: requested)
        if (hwCodecs.count(code))
            intersected.push_back(code);
    if (intersected.empty()) {
        if (!refusals.shouldSuppress("no-hw-codec-overlap", nowNs)) {
            vlog.error("No codec overlap between client preference and server HW; "
                       "falling back to image mode");
            abandonSession();
        }
        writeFallback(WEBRTC_SESSION_SCREEN, "no-hw-codec-overlap");
        return;
    }

    // Gate before any state moves: a repeated list must not rebuild transports.
    const std::string caps(payload, len);
    switch (capabilityGate.check(nowNs, caps, webRTCRequested)) {
        case webrtcpolicy::CapabilityDecision::RateLimited:
            vlog.info("ClientCapabilities from %s rate-limited; ignoring",
                host.peerAddress());
            return;
        case webrtcpolicy::CapabilityDecision::Duplicate:
            vlog.info("ClientCapabilities from %s unchanged; keeping the current session",
                host.peerAddress());
            return;
        case webrtcpolicy::CapabilityDecision::Accept:
            break;
    }
    capabilityGate.accepted(nowNs, caps);
    refusals.clear();
    imageRequestDeferred = false;
    webRTCFailedScreens.clear();
    webRTCOfferCodecs = std::move(intersected);

    if (!sentIceGeneration)
        sendIceServers(turn::CredentialSource::instance().snapshot());

    std::vector<rdr::U8> staleOffers;
    for (const auto &kv: webRTCScreens)
        if (kv.second.live.transport && kv.second.live.transport->awaitingAnswer())
            staleOffers.push_back(kv.first);
    for (rdr::U8 id: staleOffers) {
        vlog.info("Replacing unanswered WebRTC offer for screen %u after a capabilities change", id);
        host.msgWriter()->writeWebRTCClose(id, "reoffer", strlen("reoffer"));
        resetScreen(id);
    }

    const bool renegotiating = host.hasWrtcTransport();
    if (!renegotiating) {
        webRTCRequested = true;
        reconcileScreens();
        return;
    }

    vlog.info("Client %s re-advertising WebRTC capability; building pending "
              "transports for smooth codec switch",
        host.peerAddress());
    resetPendingMedia();
    beginDeferredEncoderConfig();

    std::vector<rdr::U8> liveIds;
    for (const auto &kv: webRTCScreens)
        if (kv.second.live.transport)
            liveIds.push_back(kv.first);
    std::vector<rdr::U8> offerFailed;
    for (rdr::U8 id: liveIds)
        if (!offerScreen(id, true))
            offerFailed.push_back(id);

    if (!offerFailed.empty()) {
        const KasmVideoEncoders::EncoderConfig wanted = host.connParams().pendingEncoderConfig;
        for (rdr::U8 id: offerFailed) {
            vlog.error("Could not build a pending WebRTC transport for screen %u; "
                       "falling it back to WebSocket video",
                id);
            moveScreenToWebSocket(id, "codec-switch-failed");
        }
        bool anyPending = false;
        for (const auto &kv: webRTCScreens)
            if (kv.second.pending.transport) {
                anyPending = true;
                break;
            }
        if (!anyPending && wanted.encoder != KasmVideoEncoders::Encoder::unavailable)
            commitDeferredEncoderConfig(wanted);
    }
    reconcileScreens();
}

void WebRTCController::handleIceCandidate(rdr::U8 screenId, const char *payload, size_t len) {
    auto it = webRTCScreens.find(screenId);
    if (it == webRTCScreens.end())
        return;
    WebRTCScreenSlot &slot = it->second;
    const bool liveAwaiting = slot.live.transport && slot.live.transport->awaitingAnswer();
    const bool liveConnecting = slot.live.transport && slot.live.cb &&
                                slot.live.cb->handshakeCompleteNs.load(std::memory_order_acquire) == 0;
    const bool usePending = slot.pending.transport && !liveAwaiting && !liveConnecting;
    auto *target = usePending ? slot.pending.transport.get() : slot.live.transport.get();
    auto *cb = usePending ? slot.pending.cb.get() : slot.live.cb.get();
    if (!target)
        return;
    std::string body(payload, len);
    auto pipe = body.find('|');
    if (pipe == std::string::npos) {
        vlog.error("Malformed ICE candidate (no '|' separator)");
        return;
    }
    if (!acceptRemoteCandidate(cb, len, usePending ? "pending" : "live"))
        return;
    target->addRemoteIceCandidate(body.substr(0, pipe), body.substr(pipe + 1));
}

bool WebRTCController::acceptRemoteCandidate(WebRTCCallbackState *cb, size_t len, const char *what) {
    const size_t accepted = cb ? cb->remoteCandidates : 0;
    const bool connected = cb && cb->handshakeCompleteNs.load(std::memory_order_acquire) != 0;
    switch (webrtcpolicy::checkRemoteCandidate(accepted, len, connected)) {
        case webrtcpolicy::CandidateDecision::Accept:
            break;
        case webrtcpolicy::CandidateDecision::AlreadyConnected:
            vlog.info("%s transport already connected; ignoring late ICE candidate", what);
            return false;
        case webrtcpolicy::CandidateDecision::TooLong:
            vlog.error("%s ICE candidate of %zu bytes exceeds the %zu-byte cap; dropping",
                what, len, webrtcpolicy::MAX_CANDIDATE_BYTES);
            return false;
        case webrtcpolicy::CandidateDecision::TooMany:
            vlog.error("%s ICE candidate cap (%zu) reached; dropping",
                what, webrtcpolicy::MAX_REMOTE_CANDIDATES);
            return false;
    }
    if (cb)
        cb->remoteCandidates++;
    return true;
}

void WebRTCController::abandonSession() {
    resetPendingMedia();
    if (host.hasWrtcTransport() || host.connParams().supportsWebRTCMedia)
        fallbackToImageMode(true);
    resetMedia();
}

void WebRTCController::handleImageSignal(WebRTCSignalKind k, const char *payload, size_t len) {
    switch (k) {
        case WebRTCSignalKind::ImageRequest:
            handleImageRequest();
            return;
        case WebRTCSignalKind::SdpAnswer:
            handleImageAnswer(payload, len);
            return;
        case WebRTCSignalKind::IceCandidate:
            handleImageIce(payload, len);
            return;
        case WebRTCSignalKind::Fallback:
            vlog.info("Client %s dropped the image DataChannel (%.*s)",
                host.peerAddress(),
                (int) std::min(len, (size_t) 128),
                payload ? payload : "");
            disableImageChannel("client-fallback", false);
            return;
        default:
            vlog.error("Unexpected WebRTC signal kind %u for the image channel", static_cast<unsigned>(k));
            return;
    }
}

void WebRTCController::handleImageRequest() {
    if (host.connState() != SConnection::RFBSTATE_NORMAL || !host.msgWriter())
        return;
    if (imageTransport) {
        imageRequestDeferred = false;
        return;
    }

    const int64_t nowNs = monotonicNowNs();
    if (host.connParams().supportsWebRTCMedia || webRTCRequested || host.hasWrtcTransport()) {
        if (!refusals.shouldSuppress("video-active", nowNs))
            vlog.info("Client %s asked for the image DataChannel while WebRTC video "
                      "is active; refusing",
                host.peerAddress());
        writeFallback(WEBRTC_IMAGE_SCREEN, "video-active");
        return;
    }

    if (!host.connParams().supportsLastRect) {
        if (!refusals.shouldSuppress("no-lastrect", nowNs))
            vlog.info("Client %s asked for the image DataChannel without LastRect support; refusing",
                host.peerAddress());
        writeFallback(WEBRTC_IMAGE_SCREEN, "no-lastrect");
        return;
    }

    // Deferred rather than refused: the client counts every fallback as an image-channel failure and
    // gives up after a few.
    if (webrtcpolicy::requestTooSoon(lastImageRequestNs, nowNs, WEBRTC_REQUEST_OFFER_MIN_INTERVAL_NS)) {
        if (!std::exchange(imageRequestDeferred, true))
            vlog.info("Client %s image DataChannel request rate-limited; deferring", host.peerAddress());
        if (!webrtcMaintTimer.isStarted())
            webrtcMaintTimer.start(WEBRTC_MAINT_TICK_MS);
        return;
    }
    imageRequestDeferred = false;
    lastImageRequestNs = nowNs;
    refusals.clear();

    network::WebRTCImageTransport::Config cfg;
    const auto iceSnapshot = turn::CredentialSource::instance().snapshot();
    cfg.stunUrls = iceStunUrls();
    cfg.turn = iceSnapshot.credentials;
    cfg.publicIP = static_cast<const char *>(Server::publicIP);
    const int portBegin = Server::webRTCPortRangeBegin;
    const int portEnd = Server::webRTCPortRangeEnd;
    if (portBegin > 0 && portEnd >= portBegin) {
        cfg.portRangeBegin = static_cast<uint16_t>(portBegin);
        cfg.portRangeEnd = static_cast<uint16_t>(portEnd);
    }

    sendIceServers(iceSnapshot);

    std::unique_ptr<network::WebRTCImageTransport> transport;
    try {
        transport = std::make_unique<network::WebRTCImageTransport>(cfg);
    } catch (const std::exception &e) {
        vlog.error("WebRTCImageTransport ctor threw: %s", e.what());
        writeFallback(WEBRTC_IMAGE_SCREEN, "ctor-failed");
        return;
    }

    auto cb = std::make_shared<WebRTCCallbackState>();
    cb->active.store(true, std::memory_order_release);
    transport->onOpen([cb]() {
        if (!cb->active.load(std::memory_order_acquire))
            return;
        cb->channelOpen.store(true, std::memory_order_release);
    });
    transport->onClosed([cb]() {
        if (!cb->active.load(std::memory_order_acquire))
            return;
        cb->channelClosed.store(true, std::memory_order_release);
    });
    transport->onIceStateChanged([cb](network::WebRTCImageTransport::IceState s) {
        if (!cb->active.load(std::memory_order_acquire))
            return;
        using IS = network::WebRTCImageTransport::IceState;
        if (s == IS::Failed || s == IS::Disconnected || s == IS::Closed) {
            int64_t zero = 0;
            cb->failedSinceNs.compare_exchange_strong(zero, monotonicNowNs(), std::memory_order_acq_rel);
        } else if (s == IS::Connected || s == IS::Completed) {
            cb->failedSinceNs.store(0, std::memory_order_release);
        }
    });
    transport->onLocalCandidate([cb](const std::string &candidate, const std::string &mid) {
        if (!cb->active.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lk(cb->pendingCandidatesMu);
        cb->pendingCandidates.push_back(candidate + "|" + mid);
    });

    const std::string sdp = transport->createOffer();
    if (sdp.empty()) {
        vlog.error("Image DataChannel createOffer returned empty SDP");
        cb->active.store(false, std::memory_order_release);
        writeFallback(WEBRTC_IMAGE_SCREEN, "offer-failed");
        return;
    }

    imageTransport = std::move(transport);
    imageCb = std::move(cb);
    host.msgWriter()->writeWebRTCOffer(WEBRTC_IMAGE_SCREEN, sdp.data(), sdp.size());
    vlog.info("Sent image DataChannel offer to %s", host.peerAddress());

    const int64_t offerTimeoutMs = static_cast<int64_t>((int) Server::webrtcOfferToAnswerTimeoutMs);
    imageDeadlineNs = offerTimeoutMs > 0 ? monotonicNowNs() + offerTimeoutMs * 1000000LL : 0;

    if (!webrtcMaintTimer.isStarted())
        webrtcMaintTimer.start(WEBRTC_MAINT_TICK_MS);
}

void WebRTCController::handleImageAnswer(const char *payload, size_t len) {
    if (!imageTransport)
        return;
    if (!imageTransport->handleAnswer(std::string(payload, len)))
        return;
    const int64_t connectTimeoutMs = static_cast<int64_t>((int) Server::webrtcAnswerToConnectTimeoutMs);
    imageDeadlineNs = connectTimeoutMs > 0 ? monotonicNowNs() + connectTimeoutMs * 1000000LL : 0;
}

void WebRTCController::handleImageIce(const char *payload, size_t len) {
    if (!imageTransport)
        return;
    const std::string body(payload, len);
    const auto pipe = body.find('|');
    if (pipe == std::string::npos) {
        vlog.error("Malformed image-channel ICE candidate (no '|' separator)");
        return;
    }
    if (!acceptRemoteCandidate(imageCb.get(), len, "image"))
        return;
    imageTransport->addRemoteIceCandidate(body.substr(0, pipe), body.substr(pipe + 1));
}

void WebRTCController::tickImageChannel() {
    if (!imageTransport || !imageCb)
        return;
    if (host.connState() != SConnection::RFBSTATE_NORMAL || !host.msgWriter())
        return;

    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> lk(imageCb->pendingCandidatesMu);
        out.swap(imageCb->pendingCandidates);
    }
    for (const auto &cand: out)
        host.msgWriter()->writeWebRTCIceCandidate(WEBRTC_IMAGE_SCREEN, cand.data(), cand.size());

    if (imageCb->channelClosed.load(std::memory_order_acquire)) {
        vlog.info("Client %s image DataChannel closed", host.peerAddress());
        disableImageChannel("channel-closed", true);
        return;
    }

    if (iceFailedFor(imageCb->failedSinceNs.load(std::memory_order_acquire), monotonicNowNs(), ICE_FAILURE_GRACE_NS)) {
        vlog.error("Client %s image DataChannel ICE failed for >5s; "
                   "falling back to the WebSocket",
            host.peerAddress());
        disableImageChannel("ice-timeout", true);
        return;
    }

    if (!host.connParams().supportsLastRect) {
        vlog.error("Client %s dropped LastRect support; closing the image DataChannel", host.peerAddress());
        disableImageChannel("no-lastrect", true);
        return;
    }

    if (!host.connParams().supportsImageChannel) {
        if (imageCb->channelOpen.load(std::memory_order_acquire)) {
            const size_t maxMsg = imageTransport->maxMessageSize();
            if (maxMsg && maxMsg < network::IMAGE_CHANNEL_HEADER_SIZE + network::IMAGE_CHANNEL_MIN_PIECE_SIZE) {
                vlog.error("Client %s image DataChannel max message size %zu is too small; falling back",
                    host.peerAddress(), maxMsg);
                disableImageChannel("max-message-too-small", true);
                return;
            }
            enableImageChannel();
        } else if (imageDeadlineNs && monotonicNowNs() > imageDeadlineNs) {
            vlog.error("Client %s image DataChannel handshake timed out", host.peerAddress());
            disableImageChannel("connect-timeout", true);
        }
        return;
    }

    if (host.imageStream()->takeDropped()) {
        host.markChanged(host.fullFrameRect());
        host.requestWatermark();
    }
}

void WebRTCController::enableImageChannel() {
    network::WebRTCImageTransport *const t = imageTransport.get();
    network::ImageChannelStream::Sink sink;
    sink.send = [t](const uint8_t *d, size_t n) { return t->send(d, n); };
    sink.buffered = [t]() { return t->bufferedAmount(); };
    sink.maxMessage = [t]() { return t->maxMessageSize(); };
    host.imageStream()->setSink(std::move(sink));
    host.imageStream()->takeDropped();

    imageDeadlineNs = 0;
    imageFramesSinceFull = 0;
    host.connParams().supportsImageChannel = true;
    host.connParams().recomputeCopyRect();
    host.resetEncoderZlib();
    host.markChanged(host.fullFrameRect());

    vlog.info("Client %s image rects now carried over the DataChannel", host.peerAddress());
}

void WebRTCController::resetImageChannel() {
    host.imageStream()->clearSink();
    host.imageStream()->takeDropped();
    host.connParams().supportsImageChannel = false;
    if (imageCb)
        imageCb->active.store(false, std::memory_order_release);
    imageTransport.reset();
    imageCb.reset();
    imageDeadlineNs = 0;
    imageFramesSinceFull = 0;
    lastImageFrameNs = 0;
    imageIdleMarkersSent = 0;
}

void WebRTCController::disableImageChannel(const char *reason, bool notifyClient) {
    if (!imageTransport)
        return;
    const bool wasLive = host.connParams().supportsImageChannel;

    if (notifyClient && host.msgWriter())
        writeFallback(WEBRTC_IMAGE_SCREEN, reason);
    resetImageChannel();

    if (wasLive) {
        host.connParams().recomputeCopyRect();
        host.resetEncoderZlib();
        host.markChanged(host.fullFrameRect());
    }
}

void WebRTCController::fallbackToImageMode(const bool byServer) {
    host.connParams().supportsWebRTCMedia = false;
    resetPendingMedia();
    for (const auto &kv: webRTCScreens)
        forceScreenKeyframe(kv.first);

    vlog.info("Client %s falling back from WebRTC media to image mode by %s",
        host.peerAddress(),
        byServer ? "the server" : "its own request");
}


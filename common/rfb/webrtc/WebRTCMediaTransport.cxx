/*
 * Copyright (C) 2024-2026 Kasm Technologies Corp.  All Rights Reserved.
 *
 * WebRTCMediaTransport implementation: libdatachannel-backed transport.
 * Server-side PeerConnection, single send-only video Track per session,
 * RTP packetizer chain (H264/H265/AV1 -> RtcpSrReporter ->
 * RtcpNackResponder -> PliHandler). PLI -> onKeyframeRequest.
 * negotiatedCodec() returns Unknown when negotiation fails so callers
 * fall back to WebSocket video / image mode.
 */

#include <rfb/webrtc/WebRTCMediaTransport.h>
#include <rfb/webrtc/WebRTCPublicIP.h>

#include <rfb/LogWriter.h>
#include <rfb/webrtc/RtcIceServers.h>
#include <rfb/util.h>

#include <cstring>
#include <random>
#include <sstream>

#include <rtc/av1rtppacketizer.hpp>
#include <rtc/description.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/h265rtppacketizer.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/plihandler.hpp>
#include <rtc/rtc.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/track.hpp>

using namespace network;

static rfb::LogWriter vlog("WebRTCMediaTransport");

static constexpr int PT_H264 = 96;
static constexpr int PT_H265 = 97;
static constexpr int PT_AV1 = 98;

static constexpr size_t MAX_BACKPRESSURE_BYTES = 2'000'000;

WebRTCMediaTransport::WebRTCMediaTransport(const Config &cfg) :
    config(cfg) {

    {
        std::random_device rd;
        do {
            ssrc = (static_cast<uint32_t>(rd()) << 16) ^ static_cast<uint32_t>(rd());
        } while (ssrc == 0);
    }

    rtc::Configuration rtcCfg;
    rtcCfg.iceServers = rfb::turn::rtcIceServers(cfg.stunUrls, cfg.turn);
    if (cfg.portRangeBegin && cfg.portRangeEnd) {
        rtcCfg.portRangeBegin = cfg.portRangeBegin;
        rtcCfg.portRangeEnd = cfg.portRangeEnd;
    }
    rtcCfg.forceMediaTransport = true;

    try {
        peerConnection = std::make_shared<rtc::PeerConnection>(rtcCfg);
        wireIceCallbacks();
    } catch (const std::exception &e) {
        vlog.error("WebRTCMediaTransport ctor failed (screen %u): %s", config.screenId, e.what());
        throw std::runtime_error(std::string("WebRTCMediaTransport ctor failed: ") + e.what());
    }
    vlog.info(
        "WebRTCMediaTransport constructed (codecs=%zu, ice=%zu)", config.offerCodecs.size(), config.stunUrls.size() + config.turn.urls.size());
}

WebRTCMediaTransport::~WebRTCMediaTransport() {
    shuttingDown.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(pliGuard->m);
        pliGuard->self = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(mutex);
        keyframeRequestCallback = nullptr;
        iceStateCallback = nullptr;
        trackOpenCallback = nullptr;
        localCandidateCallback = nullptr;
    }
    // libdatachannel delivers these on its own threads and they capture this, so drop them before members go.
    try {
        if (track)
            track->resetCallbacks();
        if (peerConnection)
            peerConnection->resetCallbacks();
    } catch (...) {
    }
    try {
        if (peerConnection)
            peerConnection->close();
    } catch (...) {
    }
    track.reset();
    srReporter.reset();
    rtpConfig.reset();
    peerConnection.reset();
}

rfb::KasmVideoCodecId WebRTCMediaTransport::findNegotiatedCodec(const rtc::Description &answer) const {
    for (rfb::KasmVideoCodecId want: config.offerCodecs) {
        const int wantPt = (want == rfb::kasmVideoH265) ? PT_H265 : (want == rfb::kasmVideoAV1) ? PT_AV1 : PT_H264;
        for (int i = 0; i < answer.mediaCount(); ++i) {
            auto m = answer.media(i);
            if (auto *const *ppm = std::get_if<const rtc::Description::Media *>(&m)) {
                if (!*ppm)
                    continue;
                if ((*ppm)->direction() == rtc::Description::Direction::Inactive)
                    continue;
                if ((*ppm)->hasPayloadType(wantPt))
                    return want;
            }
        }
    }
    return rfb::kasmVideoUnknownCodec;
}

void WebRTCMediaTransport::onTrackOpened() {
    framesDroppedAwaitingKey.store(0);
    awaitingKeyframe.store(true);
    trackOpen.store(true);
    vlog.info("WebRTC video track open (screen %u)", config.screenId);
    TrackOpenCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex);
        cb = trackOpenCallback;
    }
    if (cb)
        cb(config.screenId);
}

void WebRTCMediaTransport::onPliReceived() {
    plisReceived.fetch_add(1);
    KeyframeRequestCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex);
        cb = keyframeRequestCallback;
    }
    if (cb)
        cb(config.screenId);
}

void WebRTCMediaTransport::wireIceCallbacks() {
    auto self_iceCb = [this](rtc::PeerConnection::IceState s) {
        IceState mapped;
        switch (s) {
            case rtc::PeerConnection::IceState::New:
                mapped = IceState::New;
                break;
            case rtc::PeerConnection::IceState::Checking:
                mapped = IceState::Checking;
                break;
            case rtc::PeerConnection::IceState::Connected:
                mapped = IceState::Connected;
                break;
            case rtc::PeerConnection::IceState::Completed:
                mapped = IceState::Completed;
                break;
            case rtc::PeerConnection::IceState::Disconnected:
                mapped = IceState::Disconnected;
                break;
            case rtc::PeerConnection::IceState::Failed:
                mapped = IceState::Failed;
                break;
            case rtc::PeerConnection::IceState::Closed:
                mapped = IceState::Closed;
                break;
            default:
                mapped = IceState::New;
                break;
        }
        const IceState prev = iceState.exchange(mapped);
        const auto up = [](IceState x) { return x == IceState::Connected || x == IceState::Completed; };
        if (up(mapped) && !up(prev) && trackOpen.load()) {
            framesDroppedAwaitingKey.store(0);
            awaitingKeyframe.store(true, std::memory_order_release);
        }
        static const char *iceStateNames[] = {
            "New", "Checking", "Connected", "Completed", "Disconnected", "Failed", "Closed"};
        vlog.info("screen %u ICE -> %s", config.screenId, iceStateNames[static_cast<int>(mapped)]);
        IceStateCallback cb;
        {
            std::lock_guard<std::mutex> lk(mutex);
            cb = iceStateCallback;
        }
        if (cb)
            cb(mapped);
    };
    peerConnection->onIceStateChange(self_iceCb);

    peerConnection->onLocalCandidate([this](rtc::Candidate cand) {
        const std::string c = std::string(cand);
        const std::string m = cand.mid();
        LocalCandidateCallback cb;
        {
            std::lock_guard<std::mutex> lk(mutex);
            cb = localCandidateCallback;
        }
        if (!cb)
            return;
        cb(c, m);
        if (auto pub = publicIPCandidate(cand, config.publicIP))
            cb(std::string(*pub), m);
    });
}

void WebRTCMediaTransport::buildTrack(rfb::KasmVideoCodecId c, int payloadType) {
    try {
        rtc::Description::Video desc("video", rtc::Description::Direction::SendOnly);
        switch (c) {
            case rfb::kasmVideoH264:
                desc.addH264Codec(payloadType);
                break;
            case rfb::kasmVideoH265:
                desc.addH265Codec(payloadType);
                break;
            case rfb::kasmVideoAV1:
                desc.addAV1Codec(payloadType);
                break;
            default:
                vlog.error("buildTrack: unsupported codec %d for screen %u", static_cast<int>(c), config.screenId);
                return;
        }
        desc.addSSRC(ssrc, "video-send");

        track = peerConnection->addTrack(desc);

        rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            ssrc, "video-send", payloadType, rtc::H264RtpPacketizer::ClockRate);

        std::shared_ptr<rtc::MediaHandler> packetizer;
        switch (c) {
            case rfb::kasmVideoH264:
                packetizer =
                    std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfig);
                break;
            case rfb::kasmVideoH265:
                packetizer =
                    std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfig);
                break;
            case rfb::kasmVideoAV1:
                packetizer = std::make_shared<rtc::AV1RtpPacketizer>(
                    rtc::AV1RtpPacketizer::Packetization::TemporalUnit, rtpConfig);
                break;
            default:
                vlog.error("buildTrack: unsupported codec %d for screen %u", static_cast<int>(c), config.screenId);
                track.reset();
                rtpConfig.reset();
                return;
        }

        track->setMediaHandler(packetizer);

        srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
        track->chainMediaHandler(srReporter);

        // The responder counts packets, not bytes. MAX_VBV_BITS (15 Mbit) caps the rate-control buffer, so an IDR should stay near 1.9 MB, roughly
        // 1600 packets at libdatachannel's default fragment size, so leave room for the frames after it.
        constexpr size_t NACK_HISTORY_PACKETS = 2048;
        auto nack = std::make_shared<rtc::RtcpNackResponder>(NACK_HISTORY_PACKETS);
        track->chainMediaHandler(nack);

        pliGuard->self = this;
        auto pli = std::make_shared<rtc::PliHandler>([g = pliGuard]() {
            std::lock_guard<std::mutex> lk(g->m);
            if (g->self)
                g->self->onPliReceived();
        });
        track->chainMediaHandler(pli);

        track->onOpen([this]() { onTrackOpened(); });
        track->onClosed([this]() {
            trackOpen.store(false);
            vlog.info("WebRTC video track closed");
        });
    } catch (const std::exception &e) {
        vlog.error("buildTrack failed (screen %u, codec %s): %s",
            config.screenId, rfb::kasmVideoCodecName(c), e.what());
        track.reset();
        srReporter.reset();
        rtpConfig.reset();
    }
}

std::string WebRTCMediaTransport::createOffer() {
    if (config.offerCodecs.empty()) {
        vlog.error("createOffer called with no offerCodecs configured");
        return {};
    }

    rfb::KasmVideoCodecId pick = config.offerCodecs.front();
    int pt = (pick == rfb::kasmVideoH265) ? PT_H265 : (pick == rfb::kasmVideoAV1) ? PT_AV1 : PT_H264;

    try {
        buildTrack(pick, pt);
        if (!track) {
            vlog.error("createOffer: buildTrack left track unset "
                       "(screen %u, codec %s); aborting offer",
                config.screenId,
                rfb::kasmVideoCodecName(pick));
            return {};
        }
        peerConnection->setLocalDescription();
        auto desc = peerConnection->localDescription();
        if (!desc) {
            vlog.error("setLocalDescription produced no local description "
                       "(screen %u)",
                config.screenId);
            return {};
        }
        return std::string(*desc);
    } catch (const std::exception &e) {
        vlog.error("createOffer: libdatachannel threw: %s "
                   "(screen %u, codec %s)",
            e.what(),
            config.screenId,
            rfb::kasmVideoCodecName(pick));
        try {
            if (peerConnection)
                peerConnection->close();
        } catch (...) {
        }
        return {};
    }
}

void WebRTCMediaTransport::handleAnswer(const std::string &sdp) {
    answerReceived.store(true);
    try {
        rtc::Description answer(sdp, "answer");
        peerConnection->setRemoteDescription(answer);

        const rfb::KasmVideoCodecId picked = findNegotiatedCodec(answer);
        if (picked != rfb::kasmVideoUnknownCodec) {
            negotiated.store(picked);
            vlog.info("Negotiated codec: %s", rfb::kasmVideoCodecName(picked));
        } else {
            vlog.error("SDP answer has no overlap with any offered codec; "
                       "caller should fall back to image mode");
        }
    } catch (const std::exception &e) {
        vlog.error("handleAnswer failed: %s", e.what());
    }
}

void WebRTCMediaTransport::addRemoteIceCandidate(const std::string &candidate, const std::string &mid) {
    try {
        peerConnection->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception &e) {
        vlog.error("addRemoteCandidate failed: %s", e.what());
    }
}

rfb::KasmVideoCodecId WebRTCMediaTransport::negotiatedCodec() const {
    return negotiated.load();
}

bool WebRTCMediaTransport::isReady() const {
    auto s = iceState.load();
    return trackOpen.load() && (s == IceState::Connected || s == IceState::Completed);
}

uint32_t WebRTCMediaTransport::rtpTimestampNow() {
    const int64_t nowNs = rfb::monotonicNowNs();
    if (!rtpClockStarted) {
        rtpClockStartNs = nowNs;
        rtpClockStarted = true;
    }
    const int64_t elapsedNs = nowNs - rtpClockStartNs;
    const uint64_t us = elapsedNs < 0 ? 0 : static_cast<uint64_t>(elapsedNs / 1000);
    return static_cast<uint32_t>((us * 9u) / 100u);
}

bool WebRTCMediaTransport::sendEncoded(
    rfb::KasmVideoCodecId expected, uint8_t screenId, const uint8_t *data, size_t len, bool isKeyframe) {
    if (shuttingDown.load(std::memory_order_acquire))
        return false;
    if (screenId != config.screenId) {
        vlog.error("push screenId mismatch (%u vs configured %u)", screenId, config.screenId);
        return false;
    }
    const rfb::KasmVideoCodecId neg = negotiated.load();
    if (neg != expected)
        return false;
    if (!track || !track->isOpen())
        return false;

    if (!isKeyframe) {
        const size_t buffered = track->bufferedAmount();
        if (buffered > MAX_BACKPRESSURE_BYTES) {
            awaitingKeyframe.store(true, std::memory_order_release);
            framesDroppedAwaitingKey.store(0);
            return false;
        }
    }

    try {
        if (rtpConfig)
            rtpConfig->timestamp = rtpTimestampNow();
        track->send(reinterpret_cast<const std::byte *>(data), len);
        if (!diagFirstSendLogged) {
            vlog.info("[WEBRTC] screen %u: first RTP frame sent over "
                      "PeerConnection (%s, key=%d, len=%zu)",
                config.screenId,
                rfb::kasmVideoCodecName(expected),
                isKeyframe ? 1 : 0,
                len);
            diagFirstSendLogged = true;
        }
        if (isKeyframe && !diagFirstKeySent) {
            vlog.info("[WEBRTC] screen %u: first KEYFRAME sent over "
                      "PeerConnection (%s, len=%zu)",
                config.screenId,
                rfb::kasmVideoCodecName(expected),
                len);
            diagFirstKeySent = true;
        }
        return true;
    } catch (const std::exception &e) {
        vlog.error("push send failed (%s): %s", rfb::kasmVideoCodecName(expected), e.what());
        return false;
    }
}

void WebRTCMediaTransport::pushEncoded(
    uint8_t screenId, const uint8_t *nal, size_t len, bool isKeyframe) {
    const bool gated = awaitingKeyframe.load(std::memory_order_acquire);
    if (gated) {
        if (!isKeyframe) {
            if ((++framesDroppedAwaitingKey % 30) == 1) {
                KeyframeRequestCallback kf;
                {
                    std::lock_guard<std::mutex> lk(mutex);
                    kf = keyframeRequestCallback;
                }
                if (kf)
                    kf(config.screenId);
            }
            return;
        }
    }
    const rfb::KasmVideoCodecId neg = negotiatedCodec();
    switch (neg) {
        case rfb::kasmVideoH264:
        case rfb::kasmVideoH265:
        case rfb::kasmVideoAV1:
            break;
        case rfb::kasmVideoUnknownCodec:
            return;
    }
    if (sendEncoded(neg, screenId, nal, len, isKeyframe) && gated)
        awaitingKeyframe.store(false, std::memory_order_release);
}

void WebRTCMediaTransport::onKeyframeRequest(KeyframeRequestCallback cb) {
    std::lock_guard<std::mutex> lk(mutex);
    keyframeRequestCallback = std::move(cb);
}

void WebRTCMediaTransport::onIceStateChanged(IceStateCallback cb) {
    std::lock_guard<std::mutex> lk(mutex);
    iceStateCallback = std::move(cb);
}

void WebRTCMediaTransport::onTrackOpen(TrackOpenCallback cb) {
    std::lock_guard<std::mutex> lk(mutex);
    trackOpenCallback = std::move(cb);
}

void WebRTCMediaTransport::onLocalCandidate(LocalCandidateCallback cb) {
    std::lock_guard<std::mutex> lk(mutex);
    localCandidateCallback = std::move(cb);
}

WebRTCMediaTransport::Stats WebRTCMediaTransport::stats() const {
    Stats s;
    s.plisReceived = plisReceived.load();
    s.iceState = iceState.load();
    s.codec = negotiated.load();
    if (peerConnection) {
        s.bytesSent = peerConnection->bytesSent();
        s.bytesReceived = peerConnection->bytesReceived();
        if (auto r = peerConnection->rtt())
            s.rttMs = static_cast<uint32_t>(r->count());
    }
    return s;
}

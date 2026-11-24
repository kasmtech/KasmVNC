/*
 * Copyright (C) 2024-2026 Kasm Technologies Corp.  All Rights Reserved.
 *
 * This file is part of the WebRTC media-transport migration described in
 * WEBRTC_MIGRATION_PLAN_LIBDATACHANNEL.md. It is built only when
 * libdatachannel is a mandatory dependency (see third_party/CMakeLists.txt).
 *
 * Scope:
 *   - Carries encoded HW video (H.264 / H.265 / AV1) over a single
 *     RTCPeerConnection per VNC session.
 *   - Cursor, clipboard, control input, /api/ requests and SDP/ICE
 *     signaling stay on the existing WebSocket. There is intentionally
 *     no DataChannel and no audio path in this transport. Image-mode
 *     rects use the WebSocket or WebRTCImageTransport.
 *   - Image-mode fallback (Tight/JPEG/WEBP on the WebSocket) is always
 *     available — when ICE fails or no codec is negotiated, the server
 *     clears cp.supportsWebRTCMedia and the client falls back without
 *     a visible black <video> frame.
 *
 * This file is free software; redistribution and use in source and
 * binary forms with or without modification are permitted under the
 * GNU GPL v2 or later, the same as the rest of the KasmVNC tree.
 */

#ifndef __NETWORK_WEBRTC_MEDIA_TRANSPORT_H__
#define __NETWORK_WEBRTC_MEDIA_TRANSPORT_H__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rfb/webrtc/TurnConfig.h>

#include <rfb/encoders/KasmVideoConstants.h>

namespace rtc {
    class PeerConnection;
    class Track;
    class RtpPacketizationConfig;
    class RtcpSrReporter;
    class Description;
} // namespace rtc

namespace network {

    class WebRTCMediaTransport {
    public:
        struct Config {
            std::vector<std::string> stunUrls;
            rfb::turn::Credentials turn;

            std::string publicIP;

            std::vector<rfb::KasmVideoCodecId> offerCodecs = {rfb::kasmVideoH264};

            uint16_t portRangeBegin = 0;
            uint16_t portRangeEnd = 0;

            uint8_t screenId = 0;
        };

        explicit WebRTCMediaTransport(const Config &cfg);
        ~WebRTCMediaTransport();

        WebRTCMediaTransport(const WebRTCMediaTransport &) = delete;
        WebRTCMediaTransport &operator=(const WebRTCMediaTransport &) = delete;

        std::string createOffer();
        void handleAnswer(const std::string &sdp);
        void addRemoteIceCandidate(const std::string &candidate, const std::string &mid);

        rfb::KasmVideoCodecId negotiatedCodec() const;
        bool awaitingAnswer() const {
            return !answerReceived.load();
        }

        bool isReady() const;

        void pushEncoded(uint8_t screenId, const uint8_t *nal, size_t len, bool isKeyframe);

        using KeyframeRequestCallback = std::function<void(uint8_t screenId)>;
        void onKeyframeRequest(KeyframeRequestCallback cb);

        using TrackOpenCallback = std::function<void(uint8_t screenId)>;
        void onTrackOpen(TrackOpenCallback cb);

        enum class IceState
        {
            New,
            Checking,
            Connected,
            Completed,
            Disconnected,
            Failed,
            Closed
        };
        using IceStateCallback = std::function<void(IceState)>;
        void onIceStateChanged(IceStateCallback cb);

        using LocalCandidateCallback = std::function<void(const std::string &candidate, const std::string &mid)>;
        void onLocalCandidate(LocalCandidateCallback cb);

        struct Stats {
            uint64_t bytesSent = 0;
            uint64_t bytesReceived = 0;
            uint64_t packetsSent = 0;
            uint64_t plisReceived = 0;
            uint32_t rttMs = 0;
            IceState iceState = IceState::New;
            rfb::KasmVideoCodecId codec = rfb::kasmVideoUnknownCodec;
        };
        Stats stats() const;

    private:
        Config config;
        mutable std::mutex mutex;

        KeyframeRequestCallback keyframeRequestCallback;
        IceStateCallback iceStateCallback;
        TrackOpenCallback trackOpenCallback;
        LocalCandidateCallback localCandidateCallback;

        uint32_t ssrc;

        std::atomic<rfb::KasmVideoCodecId> negotiated{rfb::kasmVideoUnknownCodec};
        std::atomic<bool> answerReceived{false};
        std::atomic<IceState> iceState{IceState::New};
        std::atomic<bool> trackOpen{false};

        std::atomic<bool> awaitingKeyframe{false};

        std::atomic<bool> shuttingDown{false};

        struct PliGuard {
            std::mutex m;
            WebRTCMediaTransport *self = nullptr;
        };
        std::shared_ptr<PliGuard> pliGuard{std::make_shared<PliGuard>()};

        std::atomic<uint64_t> plisReceived{0};

        bool diagFirstSendLogged{false};
        bool diagFirstKeySent{false};
        std::atomic<uint64_t> framesDroppedAwaitingKey{0};

        std::shared_ptr<rtc::PeerConnection> peerConnection;
        std::shared_ptr<rtc::Track> track;
        std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
        std::shared_ptr<rtc::RtcpSrReporter> srReporter;

        void buildTrack(rfb::KasmVideoCodecId c, int payloadType);
        rfb::KasmVideoCodecId findNegotiatedCodec(const rtc::Description &answer) const;

        void onPliReceived();

        void onTrackOpened();

        bool sendEncoded(
            rfb::KasmVideoCodecId expected, uint8_t screenId, const uint8_t *data, size_t len, bool isKeyframe);

        void wireIceCallbacks();
        uint32_t rtpTimestampNow();

        int64_t rtpClockStartNs = 0;
        bool rtpClockStarted = false;
    };

} // namespace network

#endif

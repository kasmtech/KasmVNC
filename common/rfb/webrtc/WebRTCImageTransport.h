/*
 * Copyright (C) 2026 Kasm Technologies Corp.  All Rights Reserved.
 *
 * DataChannel-only PeerConnection that carries image-mode rects (Tight/JPEG/
 * WEBP) with UDP semantics: unordered, maxRetransmits=0. Separate from
 * WebRTCMediaTransport; a client uses one or the other, never both.
 * Signaling rides msgTypeWebRTCSignal with screenId WEBRTC_IMAGE_SCREEN.
 *
 * Threading: all public methods except the on*() registrations run on the
 * owning VNC thread. libdatachannel invokes the callbacks on its worker
 * threads, where they only touch atomics and the mutex-guarded slots.
 *
 * This file is free software under the GNU GPL v2 or later, like the rest
 * of the KasmVNC tree.
 */

#ifndef __NETWORK_WEBRTC_IMAGE_TRANSPORT_H__
#define __NETWORK_WEBRTC_IMAGE_TRANSPORT_H__

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rfb/webrtc/TurnConfig.h>

namespace rtc {
    class PeerConnection;
    class DataChannel;
} // namespace rtc

namespace network {

    class WebRTCImageTransport {
    public:
        struct Config {
            std::vector<std::string> stunUrls;
            rfb::turn::Credentials turn;
            std::string publicIP;
            uint16_t portRangeBegin = 0;
            uint16_t portRangeEnd = 0;
        };

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

        explicit WebRTCImageTransport(const Config &cfg);
        ~WebRTCImageTransport();

        WebRTCImageTransport(const WebRTCImageTransport &) = delete;
        WebRTCImageTransport &operator=(const WebRTCImageTransport &) = delete;

        std::string createOffer();
        bool handleAnswer(const std::string &sdp);
        void addRemoteIceCandidate(const std::string &candidate, const std::string &mid);

        bool isOpen() const;

        size_t maxMessageSize() const;

        size_t bufferedAmount() const;

        bool send(const uint8_t *data, size_t len);

        using VoidCallback = std::function<void()>;
        using IceStateCallback = std::function<void(IceState)>;
        using LocalCandidateCallback = std::function<void(const std::string &candidate, const std::string &mid)>;
        void onOpen(VoidCallback cb);
        void onClosed(VoidCallback cb);
        void onIceStateChanged(IceStateCallback cb);
        void onLocalCandidate(LocalCandidateCallback cb);

    private:
        Config config;
        mutable std::mutex mutex;

        VoidCallback openCallback;
        VoidCallback closedCallback;
        IceStateCallback iceStateCallback;
        LocalCandidateCallback localCandidateCallback;

        std::atomic<bool> channelOpen{false};
        std::atomic<bool> shuttingDown{false};
        std::atomic<size_t> maxMsg{0};

        std::shared_ptr<rtc::PeerConnection> peerConnection;
        std::shared_ptr<rtc::DataChannel> dataChannel;
    };

} // namespace network

#endif

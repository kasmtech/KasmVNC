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
 *
 * See WebRTCImageTransport.h.
 */

#include <rfb/webrtc/WebRTCImageTransport.h>
#include <rfb/webrtc/WebRTCPublicIP.h>

#include <rfb/LogWriter.h>
#include <rfb/webrtc/RtcIceServers.h>

#include <rtc/rtc.hpp>

using namespace network;

static rfb::LogWriter vlog("WebRTCImageTransport");

static constexpr const char *IMAGE_CHANNEL_LABEL = "kasm-image";

WebRTCImageTransport::WebRTCImageTransport(const Config &cfg) :
    config(cfg) {
    rtc::Configuration rtcCfg;
    rtcCfg.iceServers = rfb::turn::rtcIceServers(cfg.stunUrls, cfg.turn);
    if (cfg.portRangeBegin && cfg.portRangeEnd) {
        rtcCfg.portRangeBegin = cfg.portRangeBegin;
        rtcCfg.portRangeEnd = cfg.portRangeEnd;
    }

    try {
        peerConnection = std::make_shared<rtc::PeerConnection>(rtcCfg);

        peerConnection->onIceStateChange([this](rtc::PeerConnection::IceState s) {
            IceState mapped;
            switch (s) {
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
            IceStateCallback cb;
            {
                std::lock_guard<std::mutex> lk(mutex);
                cb = iceStateCallback;
            }
            if (cb)
                cb(mapped);
        });

        peerConnection->onLocalCandidate([this](rtc::Candidate cand) {
            LocalCandidateCallback cb;
            {
                std::lock_guard<std::mutex> lk(mutex);
                cb = localCandidateCallback;
            }
            if (!cb)
                return;
            cb(std::string(cand), cand.mid());
            if (auto pub = publicIPCandidate(cand, config.publicIP))
                cb(std::string(*pub), cand.mid());
        });
    } catch (const std::exception &e) {
        vlog.error("WebRTCImageTransport ctor failed: %s", e.what());
        throw std::runtime_error(std::string("WebRTCImageTransport ctor failed: ") + e.what());
    }
}

WebRTCImageTransport::~WebRTCImageTransport() {
    shuttingDown.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mutex);
        openCallback = nullptr;
        closedCallback = nullptr;
        iceStateCallback = nullptr;
        localCandidateCallback = nullptr;
    }
    // libdatachannel delivers these on its own threads and they capture this, so drop them before members go.
    try {
        if (dataChannel)
            dataChannel->resetCallbacks();
    } catch (const std::exception &e) {
        vlog.error("dc resetCallbacks failed: %s", e.what());
    } catch (...) {
        vlog.error("dc resetCallbacks failed: unknown exception");
    }
    try {
        if (peerConnection)
            peerConnection->resetCallbacks();
    } catch (const std::exception &e) {
        vlog.error("pc resetCallbacks failed: %s", e.what());
    } catch (...) {
        vlog.error("pc resetCallbacks failed: unknown exception");
    }
    try {
        if (peerConnection)
            peerConnection->close();
    } catch (const std::exception &e) {
        vlog.error("pc close failed: %s", e.what());
    } catch (...) {
        vlog.error("pc close failed: unknown exception");
    }
    dataChannel.reset();
    peerConnection.reset();
}

std::string WebRTCImageTransport::createOffer() {
    try {
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;
        dataChannel = peerConnection->createDataChannel(IMAGE_CHANNEL_LABEL, init);

        dataChannel->onOpen([this, weakDc = std::weak_ptr<rtc::DataChannel>(dataChannel)]() {
            if (shuttingDown.load(std::memory_order_acquire))
                return;
            const auto dc = weakDc.lock();
            maxMsg.store(dc ? dc->maxMessageSize() : 0);
            channelOpen.store(true, std::memory_order_release);
            VoidCallback cb;
            {
                std::lock_guard<std::mutex> lk(mutex);
                cb = openCallback;
            }
            if (cb)
                cb();
        });
        dataChannel->onClosed([this]() {
            channelOpen.store(false, std::memory_order_release);
            VoidCallback cb;
            {
                std::lock_guard<std::mutex> lk(mutex);
                cb = closedCallback;
            }
            if (cb)
                cb();
        });
        dataChannel->onMessage([](rtc::message_variant) {});

        peerConnection->setLocalDescription();
        auto desc = peerConnection->localDescription();
        if (!desc) {
            vlog.error("setLocalDescription produced no local description");
            return {};
        }
        return std::string(*desc);
    } catch (const std::exception &e) {
        vlog.error("createOffer: libdatachannel threw: %s", e.what());
        try {
            if (peerConnection)
                peerConnection->close();
        } catch (...) {
        }
        return {};
    }
}

bool WebRTCImageTransport::handleAnswer(const std::string &sdp) {
    try {
        peerConnection->setRemoteDescription(rtc::Description(sdp, "answer"));
        return true;
    } catch (const std::exception &e) {
        vlog.error("handleAnswer failed: %s", e.what());
        return false;
    }
}

void WebRTCImageTransport::addRemoteIceCandidate(const std::string &candidate, const std::string &mid) {
    try {
        peerConnection->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception &e) {
        vlog.error("addRemoteCandidate failed: %s", e.what());
    }
}

bool WebRTCImageTransport::isOpen() const {
    return channelOpen.load(std::memory_order_acquire);
}

size_t WebRTCImageTransport::maxMessageSize() const {
    return maxMsg.load();
}

size_t WebRTCImageTransport::bufferedAmount() const {
    if (shuttingDown.load(std::memory_order_acquire) || !dataChannel)
        return 0;
    try {
        return dataChannel->bufferedAmount();
    } catch (...) {
        return 0;
    }
}

bool WebRTCImageTransport::send(const uint8_t *data, size_t len) {
    if (shuttingDown.load(std::memory_order_acquire) || !dataChannel || !isOpen())
        return false;
    try {
        dataChannel->send(reinterpret_cast<const std::byte *>(data), len);
        return true;
    } catch (const std::exception &e) {
        vlog.error("DataChannel send of %zu bytes failed: %s", len, e.what());
        return false;
    }
}

void WebRTCImageTransport::onOpen(VoidCallback cb) {
    std::lock_guard<std::mutex> lk(mutex);
    openCallback = std::move(cb);
}
void WebRTCImageTransport::onClosed(VoidCallback cb) {
    std::lock_guard<std::mutex> lk(mutex);
    closedCallback = std::move(cb);
}
void WebRTCImageTransport::onIceStateChanged(IceStateCallback cb) {
    std::lock_guard<std::mutex> lk(mutex);
    iceStateCallback = std::move(cb);
}
void WebRTCImageTransport::onLocalCandidate(LocalCandidateCallback cb) {
    std::lock_guard<std::mutex> lk(mutex);
    localCandidateCallback = std::move(cb);
}

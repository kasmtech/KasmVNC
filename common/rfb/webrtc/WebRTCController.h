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

//
// WebRTCController owns the WebRTC side of one VNCSConnectionST: the per-screen
// media transports (WebRTCMediaTransport), the image DataChannel
// (WebRTCImageTransport), the signaling that negotiates them, and the timers
// that drive their handshakes and fallbacks.
//
// The controller reaches the connection only through WebRTCHost.
//

#ifndef __RFB_WEBRTCCONTROLLER_H__
#define __RFB_WEBRTCCONTROLLER_H__

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <random>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <rfb/webrtc/WebRTCImageTransport.h>
#include <rfb/webrtc/WebRTCMediaTransport.h>
#include <rfb/Rect.h>
#include <rfb/SConnection.h>
#include <rfb/Timer.h>
#include <rfb/encoders/SupportedVideoEncoders.h>
#include <rfb/webrtc/TurnCredentials.h>
#include <rfb/webrtc/WebRTCPolicy.h>
#include <rfb/webrtc/webrtcTypes.h>

namespace rfb {
  class ConnParams;
  class Region;
  class ScreenSet;
  class SMsgWriter;

  class WebRTCHost {
  public:
    virtual ConnParams &connParams() = 0;
    virtual SMsgWriter *msgWriter() = 0;
    virtual SConnection::stateEnum connState() = 0;
    virtual const char *peerAddress() = 0;
    virtual void disconnect(const char *reason) = 0;
    virtual const ScreenSet &layout() = 0;
    virtual Rect fullFrameRect() = 0;
    virtual void markChanged(const Region &region) = 0;
    virtual void sendFramebufferUpdate() = 0;
    virtual void forceScreenKeyframe(uint8_t screenId) = 0;
    virtual void setScreenMaxBitrate(uint8_t screenId, int bps) = 0;
    virtual void resetEncoderZlib() = 0;
    virtual network::ImageChannelStream *imageStream() = 0;
    virtual void attachWrtcTransport(uint8_t screenId, network::WebRTCMediaTransport *t) = 0;
    virtual bool hasWrtcTransport() const = 0;
    virtual uint8_t takeCodecMismatch() = 0;
    virtual bool takeEncoderFailed() = 0;
    virtual void requestWatermark() = 0;

  protected:
    ~WebRTCHost() = default;
  };

  class WebRTCController : public Timer::Callback {
  public:
    explicit WebRTCController(WebRTCHost &host);
    ~WebRTCController() override;

    WebRTCController(const WebRTCController &) = delete;
    WebRTCController &operator=(const WebRTCController &) = delete;

    // handleSignal() dispatches a client-to-server msgTypeWebRTCSignal.
    void handleSignal(rdr::U8 kind, rdr::U8 screenId, const char *payload, size_t len);

    // tickBeforeUpdate() runs at the start of every framebuffer update. It
    // drains ICE candidates on every call; handshake timeouts, re-offers and
    // the ICE failure fallback run only on the maintenance timer's updates.
    void tickBeforeUpdate();

    // onFramebufferUpdated() is called after an update has been encoded. It
    // schedules a trailing keyframe for WebRTC screens whose content changed.
    void onFramebufferUpdated(const Region &changed, const Region &copied);

    // Image-channel full-frame refresh (Server::udpFullFrameFrequency).
    void addImageFullFrameIfDue(Region &changed);
    void noteImageFrameSent();

    // reconcileScreens() brings the WebRTC screens in line with the
    // current layout. Called when the layout changes.
    void reconcileScreens();

    bool allScreensRoutedOverRtp() const;

    // shutdown() tears down every transport. Called from the connection's
    // destructor while the connection is still alive.
    void shutdown();

    // revokeAccess() tears down the image channel and any WebRTC media session after the
    // connection loses view rights, telling the client so it falls back.
    void revokeAccess();

  private:
    struct WebRTCCallbackState {
        std::atomic<bool> active{false};
        std::atomic<bool> pendingClientRefresh{false};
        std::atomic<int64_t> failedSinceNs{0};
        std::atomic<bool> readyToPromote{false};
        std::atomic<int64_t> handshakeCompleteNs{0};
        std::atomic<bool> channelOpen{false};
        std::atomic<bool> channelClosed{false};
        std::mutex pendingCandidatesMu;
        std::vector<std::string> pendingCandidates;
        // Remote candidates accepted so far. Touched on the connection thread only.
        size_t remoteCandidates = 0;
    };

    using WebRTCHandshakePhase = webrtcpolicy::HandshakePhase;

    bool handleTimeout(Timer *t) override;

    static constexpr int64_t ICE_FAILURE_GRACE_NS = 5'000'000'000LL;
    static constexpr auto WEBRTC_REMOVAL_DEBOUNCE = std::chrono::milliseconds(750);
    static constexpr auto WEBRTC_REMOVAL_MAX_DEFER = std::chrono::seconds(10);
    static constexpr int64_t WATERMARK_RESEND_NS = 10'000'000'000LL;
    static constexpr int MAX_WEBRTC_REOFFER_ATTEMPTS = 2;
    static constexpr int64_t WEBRTC_REQUEST_OFFER_MIN_INTERVAL_NS = 1'000'000'000LL;
    static constexpr int TRAILING_IDR_MS = 250;
    static constexpr int WEBRTC_MAINT_TICK_MS = 500;

    void writeFallback(rdr::U8 screenId, const char *reason);

    struct WebRTCLeg {
        std::unique_ptr<network::WebRTCMediaTransport> transport;
        std::shared_ptr<WebRTCCallbackState> cb;
        int64_t deadlineNs = 0;
        WebRTCHandshakePhase phase = WebRTCHandshakePhase::Idle;
    };

    struct WebRTCScreenSlot {
        WebRTCLeg live;
        WebRTCLeg pending;
    };
    std::map<rdr::U8, WebRTCScreenSlot> webRTCScreens;

    bool webRTCRequested = false;
    webrtcpolicy::CapabilityGate capabilityGate;
    std::vector<rfb::KasmVideoCodecId> webRTCOfferCodecs;
    std::optional<uint64_t> sentIceGeneration;
    uint64_t webRTCRejectedScreenIds = 0;
    KasmVideoEncoders::EncoderConfig liveWebRTCEncoderConfig{KasmVideoEncoders::Encoder::unavailable};

    void syncCallbackState();
    void clearMediaCallbacks(rdr::U8 screenId);
    void resetScreen(rdr::U8 screenId);
    void resetMedia();

    void resetPendingScreen(rdr::U8 screenId);
    void resetPendingMedia();
    void promoteMedia(rdr::U8 screenId);

    void forceScreenKeyframe(rdr::U8 screenId);
    // reason may be nullptr when the client already knows about the fallback.
    void moveScreenToWebSocket(rdr::U8 screenId, const char *reason);

    void beginDeferredEncoderConfig();
    void commitDeferredEncoderConfig(KasmVideoEncoders::EncoderConfig cfg);
    void clearDeferredEncoderConfig();

    struct WebRTCFailback {
        int64_t nextReofferNs = 0;
        int64_t capMs = 0;     // deterministic exponential ceiling
        int64_t backoffMs = 0; // jittered delay actually used
        int attempts = 0;
    };
    std::mt19937_64 backoffRng{std::random_device{}()};
    void advanceBackoff(WebRTCFailback &fb, int64_t nowNs);
    std::map<rdr::U8, WebRTCFailback> webRTCFailedScreens;
    std::map<rdr::U8, int64_t> webRTCLastRequestOfferNs;
    void markScreenFailed(rdr::U8 screenId);
    void coordinatedFallback(const char *reason);
    void abandonSession();
    void tickReoffer();
    void fallbackToImageMode(const bool byServer);

    std::map<rdr::U8, int64_t> webRTCPendingRemoval;
    void tickPendingRemoval();

    void tickHandshakeTimeout();

    void tickTrailingVideoIdr();
    bool inTrailingIdrTick{false};
    rdr::U8 trailingIdrScreens{0};
    webrtcpolicy::TrailingIdrAccumulator trailingIdrArea;

    void tickMaintenance();
    void tickWatermarkResend();
    int64_t nextWatermarkResendNs{0};

    bool offerScreen(rdr::U8 screenId, bool pending);
    bool ensureScreen(rdr::U8 screenId);
    void removeScreen(rdr::U8 screenId);
    network::WebRTCMediaTransport::Config makeConfig(rdr::U8 screenId) const;
    void sendIceServers(const turn::Snapshot &snapshot);
    void resendIceServersIfChanged();
    void wireCallbacks(network::WebRTCMediaTransport *t, rdr::U8 screenId,
        std::shared_ptr<WebRTCCallbackState> cbState, bool isPending);

    void handleClientFallback(rdr::U8 screenId, const char *payload, size_t len);
    void handleSdpAnswer(rdr::U8 screenId, const char *payload, size_t len);
    void handleIceCandidate(rdr::U8 screenId, const char *payload, size_t len);
    bool acceptRemoteCandidate(WebRTCCallbackState *cb, size_t len, const char *what);
    void handleClientCapabilities(const char *payload, size_t len);
    webrtcpolicy::RefusalMemory refusals;
    void handleRequestOffer(rdr::U8 screenId);

    std::unique_ptr<network::WebRTCImageTransport> imageTransport;
    std::shared_ptr<WebRTCCallbackState> imageCb;
    int64_t imageDeadlineNs = 0;
    void handleImageSignal(WebRTCSignalKind k, const char *payload, size_t len);
    void handleImageRequest();
    void handleImageAnswer(const char *payload, size_t len);
    void handleImageIce(const char *payload, size_t len);
    void tickImageChannel();
    void enableImageChannel();
    void disableImageChannel(const char *reason, bool notifyClient);
    void resetImageChannel();
    // The client finds a lost message only from a later sequence number, so a quiet screen gets small markers
    // after its last frame.
    void tickImageIdleMarker();
    uint32_t imageFramesSinceFull = 0;
    int64_t lastImageFrameNs = 0;
    unsigned imageIdleMarkersSent = 0;
    // Survives channel teardown, so a request, fallback, request loop stays rate-limited.
    int64_t lastImageRequestNs = 0;
    bool imageRequestDeferred = false;

    // Bit N is set for each screen N that exists in the current layout.
    uint8_t activeScreenMask() const;

    WebRTCHost &host;
    Timer trailingIdrTimer;
    Timer webrtcMaintTimer;
    bool maintDue = false;
  };
}
#endif

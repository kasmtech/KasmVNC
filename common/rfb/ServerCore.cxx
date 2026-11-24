/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
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

// -=- ServerCore.cxx

// This header will define the Server interface, from which ServerMT and
// ServerST will be derived.

#include <string.h>
#include <rfb/util.h>
#include <rfb/ServerCore.h>
#include <rfb/LogWriter.h>

namespace {
  rfb::LogWriter deprecatedLog("Config");

  // Accepts and ignores a removed parameter so existing command lines that
  // still pass it (images, templates, `vncserver` pass-through args) keep
  // starting instead of Xorg aborting on an unknown flag. Mirrors the
  // deprecation warning vncserver gives for the matching YAML keys.
  class DeprecatedParameter : public rfb::VoidParameter {
  public:
    DeprecatedParameter(const char* name_, const char* reason_)
      : rfb::VoidParameter(name_, reason_), reason(reason_) {}
    bool setParam(const char* /*value*/) override {
      deprecatedLog.error("Parameter %s is deprecated and has no effect: %s",
                          getName(), reason);
      return true;
    }
    char* getDefaultStr() const override { return nullptr; }
    char* getValueStr() const override { return rfb::strDup(""); }
  private:
    const char* reason;
  };

  // Removed with the legacy WebUDP transport (VNC-368).
  DeprecatedParameter udpPortDeprecated("udpPort",
      "legacy WebUDP transport was removed; WebRTC uses ICE-negotiated ports");
  DeprecatedParameter udpSizeDeprecated("udpSize",
      "legacy WebUDP transport was removed");
}

rfb::IntParameter rfb::Server::idleTimeout
("IdleTimeout",
 "The number of seconds after which an idle VNC connection will be dropped "
 "(zero means no timeout)",
 0, 0);
rfb::IntParameter rfb::Server::maxDisconnectionTime
("MaxDisconnectionTime",
 "Terminate when no client has been connected for s seconds",
 0, 0);
rfb::IntParameter rfb::Server::maxConnectionTime
("MaxConnectionTime",
 "Terminate when a client has been connected for s seconds",
 0, 0);
rfb::IntParameter rfb::Server::maxIdleTime
("MaxIdleTime",
 "Terminate after s seconds of user inactivity",
 0, 0);
rfb::IntParameter rfb::Server::clientWaitTimeMillis
("ClientWaitTimeMillis",
 "The number of milliseconds to wait for a client which is no longer "
 "responding",
 20000, 0);
rfb::IntParameter rfb::Server::compareFB
("CompareFB",
 "Perform pixel comparison on framebuffer to reduce unnecessary updates "
 "(0: never, 1: always, 2: auto)",
 2);
rfb::IntParameter rfb::Server::frameRate
("FrameRate",
 "The maximum number of updates per second sent to each client",
 60);
rfb::BoolParameter rfb::Server::protocol3_3
("Protocol3.3",
 "Always use protocol version 3.3 for backwards compatibility with "
 "badly-behaved clients",
 false);
rfb::BoolParameter rfb::Server::alwaysShared
("AlwaysShared",
 "Always treat incoming connections as shared, regardless of the client-"
 "specified setting",
 false);
rfb::BoolParameter rfb::Server::neverShared
("NeverShared",
 "Never treat incoming connections as shared, regardless of the client-"
 "specified setting",
 false);
rfb::BoolParameter rfb::Server::disconnectClients
("DisconnectClients",
 "Disconnect existing clients if an incoming connection is non-shared. "
 "If combined with NeverShared then new connections will be refused "
 "while there is a client active",
 true);
rfb::BoolParameter rfb::Server::acceptKeyEvents
("AcceptKeyEvents",
 "Accept key press and release events from clients.",
 true);
rfb::BoolParameter rfb::Server::acceptPointerEvents
("AcceptPointerEvents",
 "Accept pointer press and release events from clients.",
 true);
rfb::BoolParameter rfb::Server::forceGameMode
("ForceGameMode",
 "Send a message to newly connected clients instructing them to enable game mode "
 "(direct-drive relative mouse). Useful for game-focused workspace images.",
 false);
rfb::BoolParameter rfb::Server::allowTouchSupport
("AllowTouchSupport",
 "Enable touch event support for clients. When enabled, clients can send touch events "
 "for multi-touch gestures and interactions.",
 true);
rfb::BoolParameter rfb::Server::acceptCutText
("AcceptCutText",
 "Accept clipboard updates from clients.",
 true);
rfb::BoolParameter rfb::Server::sendCutText
("SendCutText",
 "Send clipboard changes to clients.",
 true);
rfb::BoolParameter rfb::Server::acceptSetDesktopSize
("AcceptSetDesktopSize",
 "Accept set desktop size events from clients.",
 true);
rfb::BoolParameter rfb::Server::queryConnect
("QueryConnect",
 "Prompt the local user to accept or reject incoming connections.",
 false);
rfb::BoolParameter rfb::Server::detectScrolling
("DetectScrolling",
 "Try to detect scrolled sections in a changed area.",
 false);
rfb::BoolParameter rfb::Server::detectHorizontal
("DetectHorizontal",
 "With -DetectScrolling enabled, try to detect horizontal scrolls too, not just vertical.",
 false);
rfb::BoolParameter rfb::Server::ignoreClientSettingsKasm
("IgnoreClientSettingsKasm",
 "Ignore the additional client settings exposed in Kasm.",
 false);
rfb::BoolParameter rfb::Server::enableLatencyMeasurement
("EnableLatencyMeasurement",
 "Allow clients to measure input-to-photon latency via round-trip measurement messages.",
 true);
rfb::BoolParameter rfb::Server::selfBench
("SelfBench",
 "Run self-benchmarks and exit.",
 false);
rfb::StringParameter rfb::Server::benchmark(
    "Benchmark",
    "Run extended benchmarks and exit.",
    "");

rfb::StringParameter rfb::Server::benchmarkResults(
    "BenchmarkResults",
    "The file to save becnhmark results to.",
    "Benchmark.xml");

rfb::IntParameter rfb::Server::dynamicQualityMin
("DynamicQualityMin",
 "The minimum dynamic JPEG quality, 0 = low, 9 = high",
 7, 0, 9);
rfb::IntParameter rfb::Server::dynamicQualityMax
("DynamicQualityMax",
 "The maximum dynamic JPEG quality, 0 = low, 9 = high",
 8, 0, 9);
rfb::IntParameter rfb::Server::treatLossless
("TreatLossless",
 "Treat lossy quality levels above and including this as lossless, 0-9. 10 = off",
 10, 0, 10);
rfb::IntParameter rfb::Server::scrollDetectLimit
("ScrollDetectLimit",
 "At least this % of the screen must change for scroll detection to happen, default 25.",
 25, 0, 100);
rfb::IntParameter rfb::Server::rectThreads
("RectThreads",
 "Use this many threads to compress rects in parallel. Default 0 (auto), 1 = off",
 0, 0, 64);
rfb::IntParameter rfb::Server::jpegVideoQuality
("JpegVideoQuality",
 "The JPEG quality to use when in video mode",
 -1, -1, 9);
rfb::IntParameter rfb::Server::webpVideoQuality
("WebpVideoQuality",
 "The WEBP quality to use when in video mode",
 -1, -1, 9);

rfb::IntParameter rfb::Server::DLP_ClipSendMax
("DLP_ClipSendMax",
 "Limit clipboard bytes to send to clients in one transaction",
 0, 0, MAX_BINARY_CLIPBOARD_SIZE);
rfb::IntParameter rfb::Server::DLP_ClipAcceptMax
("DLP_ClipAcceptMax",
 "Limit clipboard bytes to receive from clients in one transaction",
 0, 0, MAX_BINARY_CLIPBOARD_SIZE);
rfb::IntParameter rfb::Server::DLP_ClipDelay
("DLP_ClipDelay",
 "This many milliseconds must pass between clipboard actions",
 0, 0, INT_MAX);
rfb::IntParameter rfb::Server::DLP_KeyRateLimit
("DLP_KeyRateLimit",
 "Reject keyboard presses over this many per second",
 0, 0, INT_MAX);

rfb::StringParameter rfb::Server::DLP_ClipLog
("DLP_Log",
 "Log clipboard/kbd actions. Accepts off, info or verbose",
 "off");
rfb::StringParameter rfb::Server::DLP_Region
("DLP_Region",
 "Black out anything outside this region",
 "");
rfb::StringParameter rfb::Server::DLP_Clip_Types
("DLP_ClipTypes",
 "Allowed binary clipboard mimetypes",
 "text/html,image/png");

rfb::BoolParameter rfb::Server::DLP_RegionAllowClick
("DLP_RegionAllowClick",
 "Allow clicks inside the blacked-out region",
 false);
rfb::BoolParameter rfb::Server::DLP_RegionAllowRelease
("DLP_RegionAllowRelease",
 "Allow click releases inside the blacked-out region",
 true);

rfb::IntParameter rfb::Server::DLP_WatermarkRepeatSpace
("DLP_WatermarkRepeatSpace",
 "Number of pixels between repeats of the watermark",
 0, 0, 4096);
rfb::IntParameter rfb::Server::DLP_WatermarkFontSize
("DLP_WatermarkFontSize",
 "Font size for -DLP_WatermarkText",
 48, 8, 256);
rfb::IntParameter rfb::Server::DLP_WatermarkTimeOffset
("DLP_WatermarkTimeOffset",
 "Offset from UTC for -DLP_WatermarkText",
 0, -24, 24);
rfb::IntParameter rfb::Server::DLP_WatermarkTimeOffsetMinutes
("DLP_WatermarkTimeOffsetMinutes",
 "Offset from UTC for -DLP_WatermarkText, minutes",
 0, -24 * 60, 24 * 60);
rfb::IntParameter rfb::Server::DLP_WatermarkTextAngle
("DLP_WatermarkTextAngle",
 "Angle for -DLP_WatermarkText rotation",
 0, -359, 359);
rfb::StringParameter rfb::Server::DLP_WatermarkImage
("DLP_WatermarkImage",
 "PNG file to use as a watermark",
 "");
rfb::StringParameter rfb::Server::DLP_WatermarkLocation
("DLP_WatermarkLocation",
 "Place the watermark at this position from the corner.",
 "");
rfb::StringParameter rfb::Server::DLP_WatermarkTint
("DLP_WatermarkTint",
 "Tint the greyscale watermark by this color.",
 "255,255,255,255");
rfb::StringParameter rfb::Server::DLP_WatermarkText
("DLP_WatermarkText",
 "Use this text instead of an image for the watermark, with strftime time formatting",
 "");
rfb::StringParameter rfb::Server::DLP_WatermarkFont
("DLP_WatermarkFont",
 "Use this font for -DLP_WatermarkText instead of the bundled one",
 "");

rfb::StringParameter rfb::Server::maxVideoResolution
("MaxVideoResolution",
 "When in video mode, downscale the screen to max this size.",
 "1920x1080");
rfb::IntParameter rfb::Server::videoTime
("VideoTime",
 "High rate of change must happen for this many seconds to switch to video mode. "
 "Default 5, set 0 to always enable",
 5, 0, 2000);
rfb::IntParameter rfb::Server::videoOutTime
("VideoOutTime",
 "The rate of change must be below the VideoArea threshold for this many seconds "
 "to switch out of video mode.",
 3, 1, 100);
rfb::IntParameter rfb::Server::videoArea
("VideoArea",
 "High rate of change must happen for this % of the screen to switch to video mode.",
 45, 1, 100);
rfb::IntParameter rfb::Server::videoScaling
("VideoScaling",
 "Scaling method to use when in downscaled video mode. 0 = nearest, 1 = bilinear, 2 = prog bilinear",
 2, 0, 2);
rfb::BoolParameter rfb::Server::printVideoArea
("PrintVideoArea",
 "Print the detected video area % value.",
 false);
rfb::IntParameter rfb::Server::videoQualityCRFCQP
("VideoQualityCRFCQP",
 "The CRF/CPQ value to use when encoding video",
 17, 0, 63);
rfb::IntParameter rfb::Server::webRTCMaxBitrate("WebRTCMaxBitrate",
    "Upper bound (bits/sec) for WebRTC video: the fixed cap. "
    "Because the encoder runs constant-quality bounded by this cap, the stream is "
    "effectively cap-bound, so the per-frame budget is roughly this / frameRate — "
    "a too-low cap softens every frame (15Mbps/60fps was only ~31KB/frame, blurry "
    "at >=1080p). Default sized for a crisp single 1080p screen; raise for 4K / "
    "multi-monitor, lower for bandwidth-constrained deployments.",
    40000000, 100000, 200000000);
rfb::IntParameter rfb::Server::webRTCPortRangeBegin("WebRTCPortRangeBegin",
    "First local UDP port the WebRTC ICE agent may bind (with WebRTCPortRangeEnd). "
    "0 (default) lets the OS pick ephemeral ports; set both to a fixed range to "
    "open only that range in a firewall or publish it from a container.",
    0, 0, 65535);
rfb::IntParameter rfb::Server::webRTCPortRangeEnd("WebRTCPortRangeEnd",
    "Last local UDP port of the WebRTC ICE port range; see WebRTCPortRangeBegin. "
    "Each screen's PeerConnection uses at least one port, so size the range for "
    "clients x screens (x2 while a codec switch is in flight).",
    0, 0, 65535);
rfb::IntParameter rfb::Server::groupOfPicture
("GroupOfPicture",
 "The number of frames to group together for encoding",
 24, 0, 100);
rfb::StringParameter rfb::Server::driNode
("drinode",
 "Path to the hardware acceleration device (e.g. /dev/dri/renderD128)",
 "");
rfb::StringParameter rfb::Server::kasmPasswordFile
("KasmPasswordFile",
 "Password file for BasicAuth, created with the kasmvncpasswd utility.",
 "~/.kasmpasswd");

rfb::StringParameter rfb::Server::publicIP("publicIP",
    "The server's public IP. WebRTC advertises each host ICE candidate with this "
    "address as well (NAT 1:1), so a server behind a port-forwarding NAT is "
    "reachable without STUN. Loopback disables it. If not set, will be queried "
    "via the internet.",
    "");
rfb::StringParameter rfb::Server::stunServer("stunServer",
    "Use this STUN server, as hostname[:port] or stun:hostname[:port], for querying the server's "
    "public IP and as the WebRTC ICE STUN server. If not set or 'auto', a hardcoded list is used.", "");
rfb::IntParameter rfb::Server::udpFullFrameFrequency("udpFullFrameFrequency",
    "While image rects ride the unreliable WebRTC DataChannel, resend the whole "
    "framebuffer every N updates so lost rects heal even on an idle screen. "
    "0 to disable.",
    300, 0, 1000);

rfb::StringParameter rfb::Server::turnServer("TurnServer",
    "Comma-separated TURN servers for WebRTC (turn:/turns: URLs or bare hosts, "
    "optionally ?transport=udp|tcp). Requires TurnUsername and TurnCredentialFile.",
    "");

rfb::StringParameter rfb::Server::turnUsername("TurnUsername",
    "Username for the TurnServer relays.",
    "");

rfb::StringParameter rfb::Server::turnCredentialFile("TurnCredentialFile",
    "File holding the TurnServer credential. It must be a regular file with no "
    "group or other permissions; the credential is never accepted on the command line.",
    "");

rfb::StringParameter rfb::Server::turnCredentialUrl("TurnCredentialUrl",
    "HTTPS endpoint issuing short-lived TURN credentials, authorized with the JWT "
    "in the KASM_API_JWT environment variable. Overrides the static TURN settings.",
    "");

rfb::StringParameter rfb::Server::turnCredentialCaFile("TurnCredentialCaFile",
    "CA bundle used to verify TurnCredentialUrl instead of the system trust store.",
    "");

rfb::BoolParameter rfb::Server::turnCredentialSkipVerify("TurnCredentialSkipVerify",
    "Disable TLS certificate verification for TurnCredentialUrl.",
    false);

rfb::BoolParameter rfb::Server::forceTurn("ForceTurn",
    "Restrict browsers to TURN relay candidates while TURN credentials are available.",
    false);

rfb::IntParameter rfb::Server::webrtcOfferToAnswerTimeoutMs("WebRTCOfferToAnswerTimeoutMs",
    "How long (ms) the server waits for an SDP answer after sending a "
    "per-screen WebRTC offer before declaring the handshake stuck. If "
    "exceeded, the screen's handshake is considered stuck and the per-policy "
    "fallback fires (see WebRTCCoordinatedFallback). 0 disables the timeout.",
    10000, 0, 600000);

rfb::IntParameter rfb::Server::webrtcAnswerToConnectTimeoutMs("WebRTCAnswerToConnectTimeoutMs",
    "How long (ms) the server waits after a successful SDP answer for ICE "
    "to reach Connected before declaring the transport stuck. Covers stuck "
    "New/Checking states that the post-Connected IceFailureGrace path does "
    "not detect. 0 disables the timeout.",
    15000, 0, 600000);

rfb::StringParameter rfb::Server::videoCodec
("videoCodec",
 "If set, use this codec to send a video stream for WebCodecs. Supported options: auto, h264, h264_vaapi, h265, h265_vaapi, av1, av1_vaapi",
 "");

static void bandwidthPreset() {
    rfb::Server::dynamicQualityMin.setParam(2);
    rfb::Server::dynamicQualityMax.setParam(9);
    rfb::Server::treatLossless.setParam(8);
}

rfb::PresetParameter rfb::Server::preferBandwidth
("PreferBandwidth",
 "Set various options for lower bandwidth use. The default is off, aka to prefer quality.",
 false, bandwidthPreset);

rfb::IntParameter rfb::Server::webpEncodingTime
("webpEncodingTime",
 "Percentage of time allotted for encoding a frame, that can be used for encoding rects in webp.",
 30, 0, 100);

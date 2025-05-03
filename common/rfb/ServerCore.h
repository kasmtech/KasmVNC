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

// -=- ServerCore.h

// This header will define the Server interface, from which ServerMT and
// ServerST will be derived.

#ifndef __RFB_SERVER_CORE_H__
#define __RFB_SERVER_CORE_H__

#include <rfb/Configuration.h>
#include <rfb/util.h>

namespace rfb {
    class Server {
    public:
        static IntParameter idleTimeout;
        static IntParameter maxDisconnectionTime;
        static IntParameter maxConnectionTime;
        static IntParameter maxIdleTime;
        static IntParameter clientWaitTimeMillis;
        static IntParameter compareFB;
        static IntParameter frameRate;
        static IntParameter dynamicQualityMin;
        static IntParameter dynamicQualityMax;
        static IntParameter treatLossless;
        static IntParameter scrollDetectLimit;
        static IntParameter rectThreads;
        static IntParameter DLP_ClipSendMax;
        static IntParameter DLP_ClipAcceptMax;
        static IntParameter DLP_ClipDelay;
        static IntParameter DLP_KeyRateLimit;
        static IntParameter DLP_WatermarkRepeatSpace;
        static IntParameter DLP_WatermarkFontSize;
        static IntParameter DLP_WatermarkTimeOffset;
        static IntParameter DLP_WatermarkTimeOffsetMinutes;
        static IntParameter DLP_WatermarkTextAngle;
        static StringParameter DLP_ClipLog;
        static StringParameter DLP_Region;
        static StringParameter DLP_Clip_Types;
        static StringParameter DLP_WatermarkImage;
        static StringParameter DLP_WatermarkLocation;
        static StringParameter DLP_WatermarkTint;
        static StringParameter DLP_WatermarkText;
        static StringParameter DLP_WatermarkFont;
        static BoolParameter DLP_RegionAllowClick;
        static BoolParameter DLP_RegionAllowRelease;
        static IntParameter jpegVideoQuality;
        static IntParameter webpVideoQuality;
        static StringParameter maxVideoResolution;
        static IntParameter videoTime;
        static IntParameter videoOutTime;
        static IntParameter videoArea;
        static IntParameter videoScaling;
        static IntParameter udpFullFrameFrequency;
        static IntParameter udpPort;
        static StringParameter kasmPasswordFile;
        static StringParameter publicIP;
        static StringParameter stunServer;
        static BoolParameter printVideoArea;
        static BoolParameter protocol3_3;
        static BoolParameter alwaysShared;
        static BoolParameter neverShared;
        static BoolParameter disconnectClients;
        static BoolParameter acceptKeyEvents;
        static BoolParameter acceptPointerEvents;
        static BoolParameter acceptCutText;
        static BoolParameter sendCutText;
        static BoolParameter acceptSetDesktopSize;
        static BoolParameter queryConnect;
        static BoolParameter detectScrolling;
        static BoolParameter detectHorizontal;
        static BoolParameter ignoreClientSettingsKasm;
        static BoolParameter selfBench;
        static StringParameter benchmark;
        static StringParameter benchmarkResults;
        static PresetParameter preferBandwidth;
        static IntParameter webpEncodingTime;
    };
};

#endif // __RFB_SERVER_CORE_H__
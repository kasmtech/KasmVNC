/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyeight (C) 2011 D. R. Commander.  All Rights Reserved.
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
#ifndef __RFB_ENCODINGS_H__
#define __RFB_ENCODINGS_H__

namespace rfb {

  constexpr int encodingRaw = 0;
  constexpr int encodingCopyRect = 1;
  constexpr int encodingRRE = 2;
  constexpr int encodingCoRRE = 4;
  constexpr int encodingHextile = 5;
  constexpr int encodingTight = 7;
  constexpr int encodingUdp = 8;
  constexpr int encodingZRLE = 16;
  constexpr int encodingKasmVideo = 17;

  constexpr int encodingMax = 255;

  constexpr int pseudoEncodingXCursor = -240;
  constexpr int pseudoEncodingCursor = -239;
  constexpr int pseudoEncodingDesktopSize = -223;
  constexpr int pseudoEncodingLEDState = -261;
  constexpr int pseudoEncodingExtendedDesktopSize = -308;
  constexpr int pseudoEncodingDesktopName = -307;
  constexpr int pseudoEncodingFence = -312;
  constexpr int pseudoEncodingContinuousUpdates = -313;
  constexpr int pseudoEncodingCursorWithAlpha = -314;
  constexpr int pseudoEncodingQEMUKeyEvent = -258;

  // TightVNC-specific
  constexpr int pseudoEncodingLastRect = -224;
  constexpr int pseudoEncodingQualityLevel0 = -32;
  constexpr int pseudoEncodingQualityLevel9 = -23;
  constexpr int pseudoEncodingCompressLevel0 = -256;
  constexpr int pseudoEncodingCompressLevel9 = -247;

  // TurboVNC-specific
  constexpr int pseudoEncodingFineQualityLevel0 = -512;
  constexpr int pseudoEncodingFineQualityLevel100 = -412;
  constexpr int pseudoEncodingSubsamp1X = -768;
  constexpr int pseudoEncodingSubsamp4X = -767;
  constexpr int pseudoEncodingSubsamp2X = -766;
  constexpr int pseudoEncodingSubsampGray = -765;
  constexpr int pseudoEncodingSubsamp8X = -764;
  constexpr int pseudoEncodingSubsamp16X = -763;

  // Kasm-specific
  constexpr int pseudoEncodingWEBP = -1024;
  constexpr int pseudoEncodingJpegVideoQualityLevel0 = -1023;
  constexpr int pseudoEncodingJpegVideoQualityLevel9 = -1014;
  constexpr int pseudoEncodingWebpVideoQualityLevel0 = -1013;
  constexpr int pseudoEncodingWebpVideoQualityLevel9 = -1004;
  constexpr int pseudoEncodingTreatLosslessLevel0 = -1003;
  constexpr int pseudoEncodingTreatLosslessLevel10 = -993;
  constexpr int pseudoEncodingPreferBandwidth = -992;
  constexpr int pseudoEncodingDynamicQualityMinLevel0 = -991;
  constexpr int pseudoEncodingDynamicQualityMinLevel9 = -982;
  constexpr int pseudoEncodingDynamicQualityMaxLevel0 = -981;
  constexpr int pseudoEncodingDynamicQualityMaxLevel9 = -972;
  constexpr int pseudoEncodingVideoAreaLevel1 = -971;
  constexpr int pseudoEncodingVideoAreaLevel100 = -871;
  constexpr int pseudoEncodingVideoTimeLevel0 = -870;
  constexpr int pseudoEncodingVideoTimeLevel100 = -770;

  constexpr int pseudoEncodingFrameRateLevel10 = -2048;
  constexpr int pseudoEncodingFrameRateLevel60 = -1998;
  constexpr int pseudoEncodingMaxVideoResolution = -1997;
  constexpr int pseudoEncodingVideoScalingLevel0 = -1996;
  constexpr int pseudoEncodingVideoScalingLevel9 = -1987;
  constexpr int pseudoEncodingVideoOutTimeLevel1 = -1986;
  constexpr int pseudoEncodingVideoOutTimeLevel100 = -1887;
  constexpr int pseudoEncodingQOI = -1886;
  constexpr int pseudoEncodingKasmDisconnectNotify = -1885;
  constexpr int pseudoEncodingDirectMouse = -1884;

    constexpr int pseudoEncodingHardwareProfile0 = -1170;
    constexpr int pseudoEncodingHardwareProfile4 = -1166;

    constexpr int pseudoEncodingGOP1 = -1165;
    constexpr int pseudoEncodingGOP60 = -1105;
    constexpr int pseudoEncodingStreamingVideoQualityLevel0 = -1104;
    constexpr int pseudoEncodingStreamingVideoQualityLevel63 = -1041;

     // AV1
    constexpr int pseudoEncodingStreamingModeAV1QSV = -1040;
    constexpr int pseudoEncodingStreamingModeAV1NVENC = -1039;
    constexpr int pseudoEncodingStreamingModeAV1VAAPI = -1038;
    constexpr int pseudoEncodingStreamingModeAV1SW = -1037;
    constexpr int pseudoEncodingStreamingModeAV1 = -1036;
     // h.265
    constexpr int pseudoEncodingStreamingModeHEVCQSV = -1035;
    constexpr int pseudoEncodingStreamingModeHEVCNVENC = -1034;
    constexpr int pseudoEncodingStreamingModeHEVCVAAPI = -1033;
    constexpr int pseudoEncodingStreamingModeHEVCSW = -1032;
    constexpr int pseudoEncodingStreamingModeHEVC = -1031;
     // h.264
    constexpr int pseudoEncodingStreamingModeAVCQSV = -1030;
    constexpr int pseudoEncodingStreamingModeAVCNVENC = -1029;
    constexpr int pseudoEncodingStreamingModeAVCVAAPI = -1028;
    constexpr int pseudoEncodingStreamingModeAVCSW = -1027;
    constexpr int pseudoEncodingStreamingModeAVC = -1026;

    constexpr int pseudoEncodingStreamingModeJpegWebp = -1025;

  // VMware-specific
  constexpr int pseudoEncodingVMwareCursor = 0x574d5664;
  constexpr int pseudoEncodingVMwareCursorPosition = 0x574d5666;

  // UltraVNC-specific
  constexpr int pseudoEncodingExtendedClipboard = 0xC0A1E5CE;

  int encodingNum(const char* name);
  const char* encodingName(int num);
}
#endif

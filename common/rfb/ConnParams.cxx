/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
 * Copyright 2014 Pierre Ossman for Cendio AB
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
#include <stdio.h>
#include <string.h>
#include <rdr/InStream.h>
#include <rdr/OutStream.h>
#include <rfb/Exception.h>
#include <rfb/encodings.h>
#include <rfb/ledStates.h>
#include <rfb/LogWriter.h>
#include <rfb/clipboardTypes.h>
#include <rfb/ConnParams.h>
#include <rfb/ServerCore.h>
#include <rfb/SMsgHandler.h>
#include <rfb/util.h>

using namespace rfb;

static LogWriter vlog("CP");

static void clientparlog(const char name[], const bool applied) {
  vlog.debug("Client sent config param %s, %s",
             name,
             applied ? "applied" : "ignored due to -IgnoreClientSettingsKasm/lacking perms");
}
static void clientparlog(const char name[], int val, const bool applied) {
  vlog.debug("Client sent config param %s %d, %s",
             name, val,
             applied ? "applied" : "ignored due to -IgnoreClientSettingsKasm/lacking perms");
}

ConnParams::ConnParams()
  : majorVersion(0), minorVersion(0),
    width(0), height(0), useCopyRect(false),
    supportsLocalCursor(false), supportsLocalXCursor(false),
    supportsLocalCursorWithAlpha(false),
    supportsVMWareCursor(false),
    supportsCursorPosition(false),
    supportsDesktopResize(false), supportsExtendedDesktopSize(false),
    supportsDesktopRename(false), supportsLastRect(false),
    supportsLEDState(false), supportsQEMUKeyEvent(false),
    supportsWEBP(false), supportsQOI(false),
    supportsSetDesktopSize(false), supportsFence(false),
    supportsContinuousUpdates(false), supportsExtendedClipboard(false),
    supportsDisconnectNotify(false),
    supportsUdp(false),
    compressLevel(2), qualityLevel(-1), fineQualityLevel(-1),
    subsampling(subsampleUndefined), name_(0), cursorPos_(0, 0), verStrPos(0),
    ledState_(ledUnknown), shandler(NULL)
{
  memset(kasmPassed, 0, KASM_NUM_SETTINGS);
  setName("");
  cursor_ = new Cursor(0, 0, Point(), NULL);

  clipFlags = clipboardUTF8 | clipboardRTF | clipboardHTML |
              clipboardRequest | clipboardNotify | clipboardProvide;
  memset(clipSizes, 0, sizeof(clipSizes));
  clipSizes[0] = 20 * 1024 * 1024;
}

ConnParams::~ConnParams()
{
  delete [] name_;
  delete cursor_;
}

bool ConnParams::readVersion(rdr::InStream* is, bool* done)
{
  if (verStrPos >= 12) return false;
  while (is->checkNoWait(1) && verStrPos < 12) {
    verStr[verStrPos++] = is->readU8();
  }

  if (verStrPos < 12) {
    *done = false;
    return true;
  }
  *done = true;
  verStr[12] = 0;
  return (sscanf(verStr, "RFB %03d.%03d\n", &majorVersion,&minorVersion) == 2);
}

void ConnParams::writeVersion(rdr::OutStream* os)
{
  char str[13];
  sprintf(str, "RFB %03d.%03d\n", majorVersion, minorVersion);
  os->writeBytes(str, 12);
  os->flush();
}

void ConnParams::setPF(const PixelFormat& pf)
{
  pf_ = pf;

  if (pf.bpp != 8 && pf.bpp != 16 && pf.bpp != 32)
    throw Exception("setPF: not 8, 16 or 32 bpp?");
}

void ConnParams::setName(const char* name)
{
  delete [] name_;
  name_ = strDup(name);
}

void ConnParams::setCursor(const Cursor& other)
{
  delete cursor_;
  cursor_ = new Cursor(other);
}

void ConnParams::setCursorPos(const Point& pos)
{
    cursorPos_ = pos;
}

bool ConnParams::supportsEncoding(rdr::S32 encoding) const
{
  return encodings_.count(encoding) != 0;
}

void ConnParams::setEncodings(int nEncodings, const rdr::S32* encodings)
{
  useCopyRect = false;
  supportsLocalCursor = false;
  supportsLocalCursorWithAlpha = false;
  supportsVMWareCursor = false;
  supportsDesktopResize = false;
  supportsExtendedDesktopSize = false;
  supportsLocalXCursor = false;
  supportsLastRect = false;
  supportsQEMUKeyEvent = false;
  supportsWEBP = false;
  supportsQOI = false;
  supportsDisconnectNotify = false;
  compressLevel = -1;
  qualityLevel = -1;
  fineQualityLevel = -1;
  subsampling = subsampleUndefined;

  encodings_.clear();
  encodings_.insert(encodingRaw);

  bool canChangeSettings = !shandler || shandler->canChangeKasmSettings();

  for (int i = nEncodings-1; i >= 0; i--) {
    switch (encodings[i]) {
    case encodingCopyRect:
      useCopyRect = true;
      clientparlog("copyrect", true);
      break;
    case pseudoEncodingCursor:
      supportsLocalCursor = true;
      clientparlog("cursor", true);
      break;
    case pseudoEncodingXCursor:
      supportsLocalXCursor = true;
      clientparlog("xcursor", true);
      break;
    case pseudoEncodingCursorWithAlpha:
      supportsLocalCursorWithAlpha = true;
      clientparlog("cursorWithAlpha", true);
      break;
    case pseudoEncodingVMwareCursor:
      supportsVMWareCursor = true;
      clientparlog("vmwareCursor", true);
      break;
    case pseudoEncodingDesktopSize:
      supportsDesktopResize = true;
      clientparlog("desktopSize", true);
      break;
    case pseudoEncodingExtendedDesktopSize:
      supportsExtendedDesktopSize = true;
      clientparlog("extendedDesktopSize", true);
      break;
    case pseudoEncodingVMwareCursorPosition:
      supportsCursorPosition = true;
      clientparlog("vmwareCursorPosition", true);
      break;
    case pseudoEncodingDesktopName:
      supportsDesktopRename = true;
      clientparlog("desktopRename", true);
      break;
    case pseudoEncodingLastRect:
      supportsLastRect = true;
      clientparlog("lastRect", true);
      break;
    case pseudoEncodingLEDState:
      supportsLEDState = true;
      clientparlog("ledState", true);
      break;
    case pseudoEncodingQEMUKeyEvent:
      supportsQEMUKeyEvent = true;
      clientparlog("qemuKeyEvent", true);
      break;
    case pseudoEncodingWEBP:
      supportsWEBP = true;
      clientparlog("webp", true);
      break;
    case pseudoEncodingQOI:
      supportsQOI = true;
      clientparlog("qoi", true);
      break;
    case pseudoEncodingKasmDisconnectNotify:
      supportsDisconnectNotify = true;
      clientparlog("disconnectNotify", true);
      break;
    case pseudoEncodingFence:
      supportsFence = true;
      clientparlog("fence", true);
      break;
    case pseudoEncodingContinuousUpdates:
      supportsContinuousUpdates = true;
      clientparlog("continuousUpdates", true);
      break;
    case pseudoEncodingExtendedClipboard:
      supportsExtendedClipboard = true;
      clientparlog("extendedClipboard", true);
      break;
    case pseudoEncodingSubsamp1X:
      subsampling = subsampleNone;
      break;
    case pseudoEncodingSubsampGray:
      subsampling = subsampleGray;
      break;
    case pseudoEncodingSubsamp2X:
      subsampling = subsample2X;
      break;
    case pseudoEncodingSubsamp4X:
      subsampling = subsample4X;
      break;
    case pseudoEncodingSubsamp8X:
      subsampling = subsample8X;
      break;
    case pseudoEncodingSubsamp16X:
      subsampling = subsample16X;
      break;
    case pseudoEncodingPreferBandwidth:
      if (!rfb::Server::ignoreClientSettingsKasm && canChangeSettings) {
        Server::preferBandwidth.setParam(true);
        clientparlog("preferBandwidth", true);
      } else {
        clientparlog("preferBandwidth", false);
      }
      break;
    case pseudoEncodingMaxVideoResolution:
      if (!rfb::Server::ignoreClientSettingsKasm && canChangeSettings)
        kasmPassed[KASM_MAX_VIDEO_RESOLUTION] = true;
      break;
    }

    if (encodings[i] >= pseudoEncodingCompressLevel0 &&
        encodings[i] <= pseudoEncodingCompressLevel9) {
      compressLevel = encodings[i] - pseudoEncodingCompressLevel0;
      clientparlog("compressLevel", compressLevel, true);
    }

    if (encodings[i] >= pseudoEncodingQualityLevel0 &&
        encodings[i] <= pseudoEncodingQualityLevel9) {
      qualityLevel = encodings[i] - pseudoEncodingQualityLevel0;
      clientparlog("qualityLevel", qualityLevel, true);
    }

    if (encodings[i] >= pseudoEncodingFineQualityLevel0 &&
        encodings[i] <= pseudoEncodingFineQualityLevel100) {
      fineQualityLevel = encodings[i] - pseudoEncodingFineQualityLevel0;
      clientparlog("fineQualityLevel", fineQualityLevel, true);
    }

    if (!rfb::Server::ignoreClientSettingsKasm && canChangeSettings) {
      if (encodings[i] >= pseudoEncodingJpegVideoQualityLevel0 &&
          encodings[i] <= pseudoEncodingJpegVideoQualityLevel9) {
          Server::jpegVideoQuality.setParam(encodings[i] - pseudoEncodingJpegVideoQualityLevel0);
          clientparlog("jpegVideoQuality", encodings[i] - pseudoEncodingJpegVideoQualityLevel0, true);
      }

      if (encodings[i] >= pseudoEncodingWebpVideoQualityLevel0 &&
          encodings[i] <= pseudoEncodingWebpVideoQualityLevel9) {
          Server::webpVideoQuality.setParam(encodings[i] - pseudoEncodingWebpVideoQualityLevel0);
          clientparlog("webpVideoQuality", encodings[i] - pseudoEncodingWebpVideoQualityLevel0, true);
      }

      if (encodings[i] >= pseudoEncodingTreatLosslessLevel0 &&
          encodings[i] <= pseudoEncodingTreatLosslessLevel10) {
          Server::treatLossless.setParam(encodings[i] - pseudoEncodingTreatLosslessLevel0);
          clientparlog("treatLossless", encodings[i] - pseudoEncodingTreatLosslessLevel0, true);
      }

      if (encodings[i] >= pseudoEncodingDynamicQualityMinLevel0 &&
          encodings[i] <= pseudoEncodingDynamicQualityMinLevel9) {
          Server::dynamicQualityMin.setParam(encodings[i] - pseudoEncodingDynamicQualityMinLevel0);
          clientparlog("dynamicQualityMin", encodings[i] - pseudoEncodingDynamicQualityMinLevel0, true);
      }

      if (encodings[i] >= pseudoEncodingDynamicQualityMaxLevel0 &&
          encodings[i] <= pseudoEncodingDynamicQualityMaxLevel9) {
          Server::dynamicQualityMax.setParam(encodings[i] - pseudoEncodingDynamicQualityMaxLevel0);
          clientparlog("dynamicQualityMax", encodings[i] - pseudoEncodingDynamicQualityMaxLevel0, true);
      }

      if (encodings[i] >= pseudoEncodingVideoAreaLevel1 &&
          encodings[i] <= pseudoEncodingVideoAreaLevel100) {
          Server::videoArea.setParam(encodings[i] - pseudoEncodingVideoAreaLevel1 + 1);
          clientparlog("videoArea", encodings[i] - pseudoEncodingVideoAreaLevel1 + 1, true);
      }

      if (encodings[i] >= pseudoEncodingVideoTimeLevel0 &&
          encodings[i] <= pseudoEncodingVideoTimeLevel100) {
          Server::videoTime.setParam(encodings[i] - pseudoEncodingVideoTimeLevel0);
          clientparlog("videoTime", encodings[i] - pseudoEncodingVideoTimeLevel0, true);
      }

      if (encodings[i] >= pseudoEncodingVideoOutTimeLevel1 &&
          encodings[i] <= pseudoEncodingVideoOutTimeLevel100) {
          Server::videoOutTime.setParam(encodings[i] - pseudoEncodingVideoOutTimeLevel1 + 1);
          clientparlog("videoOutTime", encodings[i] - pseudoEncodingVideoOutTimeLevel1 + 1, true);
      }

      if (encodings[i] >= pseudoEncodingFrameRateLevel10 &&
          encodings[i] <= pseudoEncodingFrameRateLevel60) {
          Server::frameRate.setParam(encodings[i] - pseudoEncodingFrameRateLevel10 + 10);
          clientparlog("frameRate", encodings[i] - pseudoEncodingFrameRateLevel10 + 10, true);
      }

      if (encodings[i] >= pseudoEncodingVideoScalingLevel0 &&
          encodings[i] <= pseudoEncodingVideoScalingLevel9) {
          Server::videoScaling.setParam(encodings[i] - pseudoEncodingVideoScalingLevel0);
          clientparlog("videoScaling", encodings[i] - pseudoEncodingVideoScalingLevel0, true);
      }
    } else {
      if (encodings[i] >= pseudoEncodingJpegVideoQualityLevel0 &&
          encodings[i] <= pseudoEncodingJpegVideoQualityLevel9) {
          clientparlog("jpegVideoQuality", encodings[i] - pseudoEncodingJpegVideoQualityLevel0, false);
      }

      if (encodings[i] >= pseudoEncodingWebpVideoQualityLevel0 &&
          encodings[i] <= pseudoEncodingWebpVideoQualityLevel9) {
          clientparlog("webpVideoQuality", encodings[i] - pseudoEncodingWebpVideoQualityLevel0, false);
      }

      if (encodings[i] >= pseudoEncodingTreatLosslessLevel0 &&
          encodings[i] <= pseudoEncodingTreatLosslessLevel10) {
          clientparlog("treatLossless", encodings[i] - pseudoEncodingTreatLosslessLevel0, false);
      }

      if (encodings[i] >= pseudoEncodingDynamicQualityMinLevel0 &&
          encodings[i] <= pseudoEncodingDynamicQualityMinLevel9) {
          clientparlog("dynamicQualityMin", encodings[i] - pseudoEncodingDynamicQualityMinLevel0, false);
      }

      if (encodings[i] >= pseudoEncodingDynamicQualityMaxLevel0 &&
          encodings[i] <= pseudoEncodingDynamicQualityMaxLevel9) {
          clientparlog("dynamicQualityMax", encodings[i] - pseudoEncodingDynamicQualityMaxLevel0, false);
      }

      if (encodings[i] >= pseudoEncodingVideoAreaLevel1 &&
          encodings[i] <= pseudoEncodingVideoAreaLevel100) {
          clientparlog("videoArea", encodings[i] - pseudoEncodingVideoAreaLevel1 + 1, false);
      }

      if (encodings[i] >= pseudoEncodingVideoTimeLevel0 &&
          encodings[i] <= pseudoEncodingVideoTimeLevel100) {
          clientparlog("videoTime", encodings[i] - pseudoEncodingVideoTimeLevel0, false);
      }

      if (encodings[i] >= pseudoEncodingVideoOutTimeLevel1 &&
          encodings[i] <= pseudoEncodingVideoOutTimeLevel100) {
          clientparlog("videoOutTime", encodings[i] - pseudoEncodingVideoOutTimeLevel1 + 1, false);
      }

      if (encodings[i] >= pseudoEncodingFrameRateLevel10 &&
          encodings[i] <= pseudoEncodingFrameRateLevel60) {
          clientparlog("frameRate", encodings[i] - pseudoEncodingFrameRateLevel10 + 10, false);
      }

      if (encodings[i] >= pseudoEncodingVideoScalingLevel0 &&
          encodings[i] <= pseudoEncodingVideoScalingLevel9) {
          clientparlog("videoScaling", encodings[i] - pseudoEncodingVideoScalingLevel0, false);
      }
    }

    if (encodings[i] > 0)
      encodings_.insert(encodings[i]);
  }

  // QOI-specific overrides
  if (supportsQOI)
    useCopyRect = false;
  if (Server::DLP_WatermarkImage[0])
    useCopyRect = false;
}

void ConnParams::setLEDState(unsigned int state)
{
  ledState_ = state;
}

void ConnParams::setClipboardCaps(rdr::U32 flags, const rdr::U32* lengths)
{
  int i, num;

  clipFlags = flags;

  num = 0;
  for (i = 0;i < 16;i++) {
    if (!(flags & (1 << i)))
      continue;
    clipSizes[i] = lengths[num++];
  }
}

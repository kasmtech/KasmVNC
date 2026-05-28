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
#ifndef __RFB_MSGTYPES_H__
#define __RFB_MSGTYPES_H__

namespace rfb {
  // server to client

  constexpr int msgTypeFramebufferUpdate = 0;
  constexpr int msgTypeSetColourMapEntries = 1;
  constexpr int msgTypeBell = 2;
  constexpr int msgTypeServerCutText = 3;

  constexpr int msgTypeEndOfContinuousUpdates = 150;

  // kasm
  constexpr int msgTypeStats = 178;
  constexpr int msgTypeRequestFrameStats = 179;
  constexpr int msgTypeBinaryClipboard = 180;
  constexpr int msgTypeUpgradeToUdp = 181;
  constexpr int msgTypeSubscribeUnixRelay = 182;
  constexpr int msgTypeUnixRelay = 183;
  constexpr int msgTypeVideoEncoders = 184;
  constexpr int msgTypeKeepAlive = 185;
  constexpr int msgTypeServerDisconnect = 186;
  constexpr int msgTypeForceGameMode = 187;

  constexpr int msgTypeServerFence = 248;
  constexpr int msgTypeUserAddedToSession = 253;
  constexpr int msgTypeUserRemovedFromSession = 254;

  // client to server

  constexpr int msgTypeSetPixelFormat = 0;
  constexpr int msgTypeFixColourMapEntries = 1;
  constexpr int msgTypeSetEncodings = 2;
  constexpr int msgTypeFramebufferUpdateRequest = 3;
  constexpr int msgTypeKeyEvent = 4;
  constexpr int msgTypePointerEvent = 5;
  constexpr int msgTypeClientCutText = 6;

  constexpr int msgTypeEnableContinuousUpdates = 150;

  // kasm
  constexpr int msgTypeRequestStats = 178;
  constexpr int msgTypeFrameStats = 179;
  // same as the other direction
  //constexpr int msgTypeBinaryClipboard = 180;
  //constexpr int msgTypeUpgradeToUdp = 181;
  //constexpr int msgTypeSubscribeUnixRelay = 182;
  //constexpr int msgTypeUnixRelay = 183;
  //constexpr int msgTypeVideoEncoders = 184;
  //constexpr int msgTypeKeepAlive = 185;
  //constexpr int msgTypeServerDisconnect = 186;

  constexpr int msgTypeDirectMouseEvent = 188;

  constexpr int msgTypeClientFence = 248;

  constexpr int msgTypeSetDesktopSize = 251;

  constexpr int msgTypeSetMaxVideoResolution = 252;

  constexpr int msgTypeQEMUClientMessage = 255;
}
#endif

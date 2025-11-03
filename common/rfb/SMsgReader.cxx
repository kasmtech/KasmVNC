/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2014 Pierre Ossman for Cendio AB
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
#include <network/Udp.h>
#include <rdr/InStream.h>
#include <rdr/ZlibInStream.h>

#include <rfb/msgTypes.h>
#include <rfb/qemuTypes.h>
#include <rfb/clipboardTypes.h>
#include <rfb/Exception.h>
#include <rfb/util.h>
#include <rfb/SMsgHandler.h>
#include <rfb/SMsgReader.h>
#include <rfb/Configuration.h>
#include <rfb/LogWriter.h>
#include <rfb/ServerCore.h>

using namespace rfb;

static LogWriter vlog("SMsgReader");

SMsgReader::SMsgReader(SMsgHandler* handler_, rdr::InStream* is_)
  : handler(handler_), is(is_)
{
}

SMsgReader::~SMsgReader()
{
}

void SMsgReader::readClientInit()
{
  bool shared = is->readU8();
  handler->clientInit(shared);
}

void SMsgReader::readMsg()
{
  int msgType = is->readU8();
  switch (msgType) {
  case msgTypeSetPixelFormat:
    readSetPixelFormat();
    break;
  case msgTypeSetEncodings:
    readSetEncodings();
    break;
  case msgTypeSetDesktopSize:
    readSetDesktopSize();
    break;
  case msgTypeSetMaxVideoResolution:
    readSetMaxVideoResolution();
    break;
  case msgTypeFramebufferUpdateRequest:
    readFramebufferUpdateRequest();
    break;
  case msgTypeEnableContinuousUpdates:
    readEnableContinuousUpdates();
    break;
  case msgTypeClientFence:
    readFence();
    break;
  case msgTypeRequestStats:
    readRequestStats();
    break;
  case msgTypeFrameStats:
    readFrameStats();
    break;
  case msgTypeBinaryClipboard:
    readBinaryClipboard();
    break;
  case msgTypeKeyEvent:
    readKeyEvent();
    break;
  case msgTypePointerEvent:
    readPointerEvent();
    break;
  case msgTypeClientCutText:
    readClientCutText();
    break;
  case msgTypeQEMUClientMessage:
    readQEMUMessage();
    break;
  case msgTypeUpgradeToUdp:
    readUpgradeToUdp();
    break;
  case msgTypeSubscribeUnixRelay:
    readSubscribeUnixRelay();
    break;
  case msgTypeUnixRelay:
    readUnixRelay();
    break;
  case msgTypeKeepAlive:
    readKeepAlive();
    break;
  default:
    fprintf(stderr, "unknown message type %d\n", msgType);
    throw Exception("unknown message type");
  }
}

void SMsgReader::readSetPixelFormat()
{
  is->skip(3);
  PixelFormat pf;
  pf.read(is);
  handler->setPixelFormat(pf);
}

void SMsgReader::readSetEncodings()
{
  is->skip(1);
  int nEncodings = is->readU16();
  rdr::S32Array encodings(nEncodings);
  for (int i = 0; i < nEncodings; i++)
    encodings.buf[i] = is->readU32();
  handler->setEncodings(nEncodings, encodings.buf);
}

void SMsgReader::readSetDesktopSize()
{
  int width, height;
  int screens, i;
  rdr::U32 id, flags;
  int sx, sy, sw, sh;
  ScreenSet layout;

  is->skip(1);

  width = is->readU16();
  height = is->readU16();

  screens = is->readU8();
  is->skip(1);

  for (i = 0;i < screens;i++) {
    id = is->readU32();
    sx = is->readU16();
    sy = is->readU16();
    sw = is->readU16();
    sh = is->readU16();
    flags = is->readU32();

    layout.add_screen(Screen(id, sx, sy, sw, sh, flags));
  }

  handler->setDesktopSize(width, height, layout);
}

void SMsgReader::readSetMaxVideoResolution()
{
  unsigned int width, height;
  char tmp[16];

  width = is->readU16();
  height = is->readU16();

  if (!rfb::Server::ignoreClientSettingsKasm && handler->canChangeKasmSettings()) {
    sprintf(tmp, "%ux%u", width, height);
    rfb::Server::maxVideoResolution.setParam(tmp);
    vlog.debug("Client sent config param maxVideoResolution %ux%u, applied", width, height);
  } else {
    vlog.debug("Client sent config param maxVideoResolution %ux%u, ignored due to -IgnoreClientSettingsKasm/lacking perms", width, height);
  }
}

void SMsgReader::readFramebufferUpdateRequest()
{
  bool inc = is->readU8();
  int x = is->readU16();
  int y = is->readU16();
  int w = is->readU16();
  int h = is->readU16();
  handler->framebufferUpdateRequest(Rect(x, y, x+w, y+h), inc);
}

void SMsgReader::readEnableContinuousUpdates()
{
  bool enable;
  int x, y, w, h;

  enable = is->readU8();

  x = is->readU16();
  y = is->readU16();
  w = is->readU16();
  h = is->readU16();

  handler->enableContinuousUpdates(enable, x, y, w, h);
}

void SMsgReader::readFence()
{
  rdr::U32 flags;
  rdr::U8 len;
  char data[64];

  is->skip(3);

  flags = is->readU32();

  len = is->readU8();
  if (len > sizeof(data)) {
    fprintf(stderr, "Ignoring fence with too large payload\n");
    is->skip(len);
    return;
  }

  is->readBytes(data, len);
  
  handler->fence(flags, len, data);
}

void SMsgReader::readKeyEvent()
{
  bool down = is->readU8();
  is->skip(2);
  rdr::U32 key = is->readU32();
  handler->keyEvent(key, 0, down);
}

void SMsgReader::readPointerEvent()
{
  int mask = is->readU16();
  int x = is->readU16();
  int y = is->readU16();
  int scrollX = is->readS16();
  int scrollY = is->readS16();
  
  handler->pointerEvent(Point(x, y), Point(0, 0), mask, false, false, scrollX, scrollY);
}


void SMsgReader::readClientCutText()
{
  is->skip(3);
  rdr::U32 len = is->readU32();

  if (len & 0x80000000) {
    rdr::S32 slen = len;
    slen = -slen;
    readExtendedClipboard(slen);
    return;
  }
  is->skip(len);
  vlog.error("Client sent old cuttext msg, ignoring");
}

void SMsgReader::readBinaryClipboard()
{
  const rdr::U8 num = is->readU8();
  rdr::U8 i, valid = 0;
  char tmpmimes[num][32];

  handler->clearBinaryClipboard();
  for (i = 0; i < num; i++) {
    const rdr::U8 mimelen = is->readU8();
    if (mimelen > 32 - 1) {
      vlog.error("Mime too long (%u)", mimelen);
    }

    char mime[mimelen + 1];
    mime[mimelen] = '\0';
    is->readBytes(mime, mimelen);

    strncpy(tmpmimes[valid], mime, 32);
    tmpmimes[valid][31] = '\0';

    const rdr::U32 len = is->readU32();
    CharArray ca(len);
    is->readBytes(ca.buf, len);

    if (rfb::Server::DLP_ClipAcceptMax && len > (unsigned) rfb::Server::DLP_ClipAcceptMax) {
      vlog.info("DLP: refused to receive binary clipboard, too large");
      continue;
    }

    vlog.debug("Received binary clipboard, type %s, %u bytes", mime, len);

    handler->addBinaryClipboard(mime, (rdr::U8 *) ca.buf, len, 0);
    valid++;
  }

  handler->handleClipboardAnnounceBinary(valid, tmpmimes);
}

void SMsgReader::readExtendedClipboard(rdr::S32 len)
{
  if (len < 4)
    throw Exception("Invalid extended clipboard message");
  vlog.error("Client sent old cuttext msg, ignoring");
  is->skip(len);
}

void SMsgReader::readRequestStats()
{
  is->skip(3);
  handler->sendStats();
}

void SMsgReader::readFrameStats()
{
  is->skip(3);
  rdr::U32 all = is->readU32();
  rdr::U32 render = is->readU32();
  handler->handleFrameStats(all, render);
}

void SMsgReader::readKeepAlive()
{
  handler->keepAlive();
}

void SMsgReader::readQEMUMessage()
{
  int subType = is->readU8();
  switch (subType) {
  case qemuExtendedKeyEvent:
    readQEMUKeyEvent();
    break;
  default:
    throw Exception("unknown QEMU submessage type %d", subType);
  }
}

void SMsgReader::readQEMUKeyEvent()
{
  bool down = is->readU16();
  rdr::U32 keysym = is->readU32();
  rdr::U32 keycode = is->readU32();
  if (!keycode) {
    vlog.error("Key event without keycode - ignoring");
    return;
  }
  handler->keyEvent(keysym, keycode, down);
}

void SMsgReader::readUpgradeToUdp()
{
  char buf[4096], resp[4096];
  rdr::U16 len = is->readU16();

  if (len >= sizeof(buf)) {
    vlog.error("Ignoring udp upgrade with too large payload");
    is->skip(len);
    return;
  }

  if (!len) {
    handler->udpDowngrade(false);
    return;
  }

  is->readBytes(buf, len);
  buf[len] = '\0';

  wuGotHttp(buf, len, resp);

  handler->udpUpgrade(resp);
}

void SMsgReader::readSubscribeUnixRelay()
{
  const rdr::U8 namelen = is->readU8();
  char name[64];
  if (namelen >= sizeof(name)) {
    vlog.error("Ignoring subscribe with too large name");
    is->skip(namelen);
    return;
  }
  is->readBytes(name, namelen);
  name[namelen] = '\0';

  handler->subscribeUnixRelay(name);
}

void SMsgReader::readUnixRelay()
{
  const rdr::U8 namelen = is->readU8();
  char name[64];
  if (namelen >= sizeof(name)) {
    vlog.error("Ignoring relay packet with too large name");
    is->skip(namelen);
    return;
  }
  is->readBytes(name, namelen);
  name[namelen] = '\0';

  const rdr::U32 len = is->readU32();
  rdr::U8 buf[1024 * 1024];
  if (len >= sizeof(buf)) {
    vlog.error("Ignoring relay packet with too large data");
    is->skip(len);
    return ;
  }
  is->readBytes(buf, len);

  handler->unixRelay(name, buf, len);
}

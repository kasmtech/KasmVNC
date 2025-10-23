/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2011-2015 Pierre Ossman for Cendio AB
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

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <set>
#include <string>
#include <vector>

#include <rfb/Configuration.h>
#include <rfb/Logger_stdio.h>
#include <rfb/LogWriter.h>
#include <rfb/util.h>
#include <rfb/ServerCore.h>
#include <rdr/HexOutStream.h>
#include <rfb/LogWriter.h>
#include <rfb/Hostname.h>
#include <rfb/Region.h>
#include <rfb/ledStates.h>
#include <rfb/Watermark.h>
#include <network/iceip.h>
#include <network/TcpSocket.h>
#include <network/UnixSocket.h>

#include "XserverDesktop.h"
#include "vncExtInit.h"
#include "vncHooks.h"
#include "vncBlockHandler.h"
#include "vncSelection.h"
#include "XorgGlue.h"
#include "RandrGlue.h"
#include "xorg-version.h"

extern "C" {
void vncSetGlueContext(int screenIndex);

int wakeuppipe[2];
}

using namespace rfb;

static rfb::LogWriter vlog("vncext");

// We can't safely get this from Xorg
#define MAXSCREENS 16

static unsigned long vncExtGeneration = 0;
static bool initialised = false;
static XserverDesktop* desktop[MAXSCREENS] = { 0, };
void* vncFbptr[MAXSCREENS] = { 0, };
int vncFbstride[MAXSCREENS];

int vncInetdSock = -1;

static std::vector<dlp_mimetype_t> dlp_mimetypes;

struct CaseInsensitiveCompare {
  bool operator() (const std::string &a, const std::string &b) const {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  }
};

typedef std::set<std::string, CaseInsensitiveCompare> ParamSet;
static ParamSet allowOverrideSet;

rfb::AliasParameter rfbwait("rfbwait", "Alias for ClientWaitTimeMillis",
                            &rfb::Server::clientWaitTimeMillis);
rfb::IntParameter rfbport("rfbport", "TCP port to listen for RFB protocol",0);
rfb::StringParameter rfbunixpath("rfbunixpath", "Unix socket to listen for RFB protocol", "");
rfb::IntParameter rfbunixmode("rfbunixmode", "Unix socket access mode", 0600);
rfb::StringParameter desktopName("desktop", "Name of VNC desktop","x11");
rfb::BoolParameter localhostOnly("localhost",
                                 "Only allow connections from localhost",
                                 false);
rfb::BoolParameter noWebsocket("noWebsocket",
                                 "Listen on a traditional VNC port instead of websocket",
                                 false);
rfb::IntParameter websocketPort("websocketPort", "websocket port to listen for", 6800);
rfb::StringParameter cert("cert", "SSL pem cert to use for websocket connections", "");
rfb::StringParameter certkey("key", "SSL pem key to use for websocket connections (if separate)", "");
rfb::BoolParameter sslonly("sslOnly", "Require SSL for websockets", false);
rfb::BoolParameter disablebasicauth("DisableBasicAuth", "Disable basic auth for websockets", false);
rfb::StringParameter interface("interface",
                               "listen on the specified network address",
                               "all");
rfb::BoolParameter avoidShiftNumLock("AvoidShiftNumLock",
                                     "Avoid fake Shift presses for keys affected by NumLock.",
                                     true);
rfb::StringParameter allowOverride("AllowOverride",
                                   "Comma separated list of parameters that can be modified using VNC extension.",
                                   "desktop,AcceptPointerEvents,SendCutText,AcceptCutText,SendPrimary,SetPrimary");
rfb::BoolParameter setPrimary("SetPrimary", "Set the PRIMARY as well "
                              "as the CLIPBOARD selection", true);
rfb::BoolParameter sendPrimary("SendPrimary",
                               "Send the PRIMARY as well as the CLIPBOARD selection",
                               false);

static PixelFormat vncGetPixelFormat(int scrIdx)
{
  int depth, bpp;
  int trueColour, bigEndian;
  int redMask, greenMask, blueMask;

  int redShift, greenShift, blueShift;
  int redMax, greenMax, blueMax;

  vncGetScreenFormat(scrIdx, &depth, &bpp, &trueColour, &bigEndian,
                     &redMask, &greenMask, &blueMask);

  if (!trueColour) {
    vlog.error("pseudocolour not supported");
    abort();
  }

  redShift   = ffs(redMask) - 1;
  greenShift = ffs(greenMask) - 1;
  blueShift  = ffs(blueMask) - 1;
  redMax     = redMask   >> redShift;
  greenMax   = greenMask >> greenShift;
  blueMax    = blueMask  >> blueShift;

  return PixelFormat(bpp, depth, bigEndian, trueColour,
                     redMax, greenMax, blueMax,
                     redShift, greenShift, blueShift);
}

static void parseOverrideList(const char *text, ParamSet &out)
{
  for (const char* iter = text; ; ++iter) {
    if (*iter == ',' || *iter == '\0') {
      out.insert(std::string(text, iter));
      text = iter + 1;

      if (*iter == '\0')
        break;
    }
  }
}

static void parseClipTypes()
{
  char *str = strdup(Server::DLP_Clip_Types);
  char * const origstr = str;

  while (1) {
    char *cur = strsep(&str, ",");
    if (!cur)
      break;
    if (!cur[0])
      continue;

    // Hardcoded filters
    if (!strcmp(cur, "TEXT") ||
        !strcmp(cur, "STRING") ||
        strstr(cur, "text/plain") ||
        !strcmp(cur, "UTF8_STRING"))
      continue;

    struct dlp_mimetype_t m;
    strncpy(m.mime, cur, sizeof(m.mime));
    m.mime[sizeof(m.mime) - 1] = '\0';

    dlp_mimetypes.push_back(m);

    vlog.debug("Adding DLP binary mime type %s", m.mime);
  }
  vlog.debug("Total %lu binary mime types", dlp_mimetypes.size());

  free(origstr);
}

void vncExtensionInit(void)
{
  if (vncExtGeneration == vncGetServerGeneration()) {
    vlog.error("vncExtensionInit: called twice in same generation?");
    return;
  }
  vncExtGeneration = vncGetServerGeneration();

  if (vncGetScreenCount() > MAXSCREENS)
    vncFatalError("vncExtensionInit: too many screens\n");

  if (sizeof(ShortRect) != sizeof(struct UpdateRect))
    vncFatalError("vncExtensionInit: Incompatible ShortRect size\n");

  vncAddExtension();

  if (!initialised) {
    parseClipTypes();
    getPublicIP();
  }
  vncSelectionInit();

  vlog.info("VNC extension running!");

  try {
    if (!initialised) {
      rfb::initStdIOLoggers();

      parseOverrideList(allowOverride, allowOverrideSet);
      allowOverride.setImmutable();

      if (!Server::DLP_ClipLog[0] ||
          (strcmp(Server::DLP_ClipLog, "off") &&
           strcmp(Server::DLP_ClipLog, "info") &&
           strcmp(Server::DLP_ClipLog, "verbose")))
           vncFatalError("Invalid value to %s", Server::DLP_ClipLog.getName());

      unsigned dummyX, dummyY;
      if (!Server::maxVideoResolution[0] ||
          sscanf(Server::maxVideoResolution, "%ux%u", &dummyX, &dummyY) != 2 ||
          dummyX < 16 ||
          dummyY < 16)
          vncFatalError("Invalid value to %s", Server::maxVideoResolution.getName());

      if (!watermarkInit())
          vncFatalError("Invalid watermark params");

      pipe(wakeuppipe);
      const int flags = fcntl(wakeuppipe[0], F_GETFL, 0);
      fcntl(wakeuppipe[0], F_SETFL, flags | O_NONBLOCK);
      vncSetNotifyFd(wakeuppipe[0], 0, true, false);

      unsigned i;
      for (i = 0; i < MAX_UNIX_RELAYS; i++) {
          if (unixrelays[i] == -1)
              break;
          vncSetNotifyFd(unixrelays[i], 0, true, false);
          vlog.info("Listening to unix relay socket %s", unixrelaynames[i]);
      }

      initialised = true;
    }

    for (int scr = 0; scr < vncGetScreenCount(); scr++) {

      if (!desktop[scr]) {
        std::list<network::SocketListener*> listeners;
        if (scr == 0 && vncInetdSock != -1) {
          if (network::isSocketListening(vncInetdSock))
          {
            listeners.push_back(new network::TcpListener(vncInetdSock));
            vlog.info("inetd wait");
          }
        } else if (rfbunixpath.getValueStr()[0] != '\0') {
          char path[PATH_MAX];
          int mode = (int)rfbunixmode;

          if (scr == 0)
            strncpy(path, rfbunixpath, sizeof(path));
          else
            snprintf(path, sizeof(path), "%s.%d",
                     rfbunixpath.getValueStr(), scr);
          path[sizeof(path)-1] = '\0';

          listeners.push_back(new network::UnixListener(path, mode));

          vlog.info("Listening for VNC connections on %s (mode %04o)",
                    path, mode);
        } else {
          const char *addr = interface;
          int port = rfbport;
          if (port == 0) port = 5900 + atoi(vncGetDisplay());
          port += 1000 * scr;
          if (strcasecmp(addr, "all") == 0)
            addr = 0;
          if (!noWebsocket)
            network::createWebsocketListeners(&listeners, websocketPort,
                                              localhostOnly ? "local" : addr,
                                              sslonly, cert, certkey, disablebasicauth, httpDir);
          else if (localhostOnly)
            network::createLocalTcpListeners(&listeners, port);
          else
            network::createTcpListeners(&listeners, addr, port);

          if (noWebsocket)
            vlog.info("Listening for VNC connections on %s interface(s), port %d",
                    localhostOnly ? "local" : (const char*)interface,
                    port);
          else
            vlog.info("Listening for websocket connections on %s interface(s), port %d",
                    localhostOnly ? "local" : (const char*)interface,
                    (int) websocketPort);
        }

        CharArray desktopNameStr(desktopName.getData());
        PixelFormat pf = vncGetPixelFormat(scr);

        vncSetGlueContext(scr);
        desktop[scr] = new XserverDesktop(scr,
                                          listeners,
                                          desktopNameStr.buf,
                                          pf,
                                          vncGetScreenWidth(),
                                          vncGetScreenHeight(),
                                          vncFbptr[scr],
                                          vncFbstride[scr]);
        vlog.info("created VNC server for screen %d", scr);

        if (scr == 0 && vncInetdSock != -1 && listeners.empty()) {
          network::Socket* sock = new network::TcpSocket(vncInetdSock);
          desktop[scr]->addClient(sock, false);
          vlog.info("added inetd sock");
        }
      }

      vncHooksInit(scr);
    }
  } catch (rdr::Exception& e) {
    vncFatalError("vncExtInit: %s\n",e.str());
  }

  vncRegisterBlockHandlers();
}

void vncExtensionClose(void)
{
  try {
    for (int scr = 0; scr < vncGetScreenCount(); scr++) {
      delete desktop[scr];
      desktop[scr] = NULL;
    }
  } catch (rdr::Exception& e) {
    vncFatalError("vncExtInit: %s\n",e.str());
  }
}

void vncHandleSocketEvent(int fd, int scrIdx, int read, int write)
{
  desktop[scrIdx]->handleSocketEvent(fd, read, write);
}

void vncCallBlockHandlers(int* timeout)
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->blockHandler(timeout);
}

int vncGetAvoidShiftNumLock(void)
{
  return (bool)avoidShiftNumLock;
}

int vncGetSetPrimary(void)
{
  return (bool)setPrimary;
}

int vncGetSendPrimary(void)
{
  return (bool)sendPrimary;
}

void vncUpdateDesktopName(void)
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->setDesktopName(desktopName);
}

void vncAnnounceClipboard(int available)
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->announceClipboard(available);
}

void vncSendBinaryClipboardData(const char* mime, const unsigned char *data,
                                const unsigned len)
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->sendBinaryClipboardData(mime, data, len);
}

void vncGetBinaryClipboardData(const char *mime, const unsigned char **ptr,
                               unsigned *len)
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->getBinaryClipboardData(mime, ptr, len);
}

void vncClearBinaryClipboardData()
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->clearBinaryClipboardData();
}

int vncConnectClient(const char *addr)
{
  if (strlen(addr) == 0) {
    try {
      desktop[0]->disconnectClients();
    } catch (rdr::Exception& e) {
      vlog.error("Disconnecting all clients: %s",e.str());
      return -1;
    }
    return 0;
  }

  char *host;
  int port;

  getHostAndPort(addr, &host, &port, 5500);

  try {
    network::Socket* sock = new network::TcpSocket(host, port);
    delete [] host;
    desktop[0]->addClient(sock, true);
  } catch (rdr::Exception& e) {
    vlog.error("Reverse connection: %s",e.str());
    return -1;
  }

  return 0;
}

void vncGetQueryConnect(uint32_t *opaqueId, const char**username,
                        const char **address, int *timeout)
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++) {
    desktop[scr]->getQueryConnect(opaqueId, username, address, timeout);
    if (opaqueId != 0)
      break;
  }
}

void vncApproveConnection(uint32_t opaqueId, int approve)
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++) {
    desktop[scr]->approveConnection(opaqueId, approve,
                                    "Connection rejected by local user");
  }
}

void vncBell()
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->bell();
}

void vncSetLEDState(unsigned long leds)
{
  unsigned int state;

  state = 0;
  if (leds & (1 << 0))
    state |= ledCapsLock;
  if (leds & (1 << 1))
    state |= ledNumLock;
  if (leds & (1 << 2))
    state |= ledScrollLock;

  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->setLEDState(state);
}

void vncAddChanged(int scrIdx, const struct UpdateRect *extents,
                   int nRects, const struct UpdateRect *rects)
{
  Region reg;

  reg.setExtentsAndOrderedRects((const ShortRect*)extents,
                                nRects, (const ShortRect*)rects);
  desktop[scrIdx]->add_changed(reg);
}

void vncAddCopied(int scrIdx, const struct UpdateRect *extents,
                  int nRects, const struct UpdateRect *rects,
                  int dx, int dy)
{
  Region reg;

  reg.setExtentsAndOrderedRects((const ShortRect*)extents,
                                nRects, (const ShortRect*)rects);
  desktop[scrIdx]->add_copied(reg, rfb::Point(dx, dy));
}

void vncSetCursor(int width, int height, int hotX, int hotY,
                  const unsigned char *rgbaData)
{
  for (int scr = 0; scr < vncGetScreenCount(); scr++)
    desktop[scr]->setCursor(width, height, hotX, hotY, rgbaData);
}

void vncSetCursorPos(int scrIdx, int x, int y)
{
  desktop[scrIdx]->setCursorPos(x, y, true);
}

void vncPreScreenResize(int scrIdx)
{
  // We need to prevent the RFB core from accessing the framebuffer
  // for a while as there might be updates thrown our way inside
  // the routines that change the screen (i.e. before we have a
  // pointer to the new framebuffer).
  desktop[scrIdx]->blockUpdates();
}

void vncPostScreenResize(int scrIdx, int success, int width, int height)
{
  if (success) {
    // Let the RFB core know of the new dimensions and framebuffer
    try {
      desktop[scrIdx]->setFramebuffer(width, height,
                                      vncFbptr[scrIdx],
                                      vncFbstride[scrIdx]);
    } catch (rdr::Exception& e) {
      vncFatalError("vncPostScreenResize: %s\n", e.str());
    }
  }

  desktop[scrIdx]->unblockUpdates();

  if (success) {
    // Mark entire screen as changed
    desktop[scrIdx]->add_changed(Region(Rect(0, 0, width, height)));
  }
}

void vncRefreshScreenLayout(int scrIdx)
{
  try {
    desktop[scrIdx]->refreshScreenLayout();
  } catch (rdr::Exception& e) {
    vncFatalError("vncRefreshScreenLayout: %s\n", e.str());
  }
}

int vncOverrideParam(const char *nameAndValue)
{
  const char* equalSign = strchr(nameAndValue, '=');
  if (!equalSign)
    return 0;

  std::string key(nameAndValue, equalSign);
  if (allowOverrideSet.find(key) == allowOverrideSet.end())
    return 0;

  return rfb::Configuration::setParam(nameAndValue);
}

unsigned dlp_num_mimetypes()
{
  return dlp_mimetypes.size();
}

const char *dlp_get_mimetype(const unsigned i)
{
  return dlp_mimetypes[i].mime;
}

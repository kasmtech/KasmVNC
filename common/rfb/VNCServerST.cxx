/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2018 Pierre Ossman for Cendio AB
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

// -=- Single-Threaded VNC Server implementation


// Note about how sockets get closed:
//
// Closing sockets to clients is non-trivial because the code which calls
// VNCServerST must explicitly know about all the sockets (so that it can block
// on them appropriately).  However, VNCServerST may want to close clients for
// a number of reasons, and from a variety of entry points.  The simplest is
// when processSocketEvent() is called for a client, and the remote end has
// closed its socket.  A more complex reason is when processSocketEvent() is
// called for a client which has just sent a ClientInit with the shared flag
// set to false - in this case we want to close all other clients.  Yet another
// reason for disconnecting clients is when the desktop size has changed as a
// result of a call to setPixelBuffer().
//
// The responsibility for creating and deleting sockets is entirely with the
// calling code.  When VNCServerST wants to close a connection to a client it
// calls the VNCSConnectionST's close() method which calls shutdown() on the
// socket.  Eventually the calling code will notice that the socket has been
// shut down and call removeSocket() so that we can delete the
// VNCSConnectionST.  Note that the socket must not be deleted by the calling
// code until after removeSocket() has been called.
//
// One minor complication is that we don't allocate a VNCSConnectionST object
// for a blacklisted host (since we want to minimise the resources used for
// dealing with such a connection).  In order to properly implement the
// getSockets function, we must maintain a separate closingSockets list,
// otherwise blacklisted connections might be "forgotten".


#include <assert.h>
#include <stdlib.h>

#include <network/GetAPI.h>

#include <rfb/ComparingUpdateTracker.h>
#include <rfb/KeyRemapper.h>
#include <rfb/ListConnInfo.h>
#include <rfb/Security.h>
#include <rfb/ServerCore.h>
#include <rfb/VNCServerST.h>
#include <rfb/VNCSConnectionST.h>
#include <rfb/util.h>
#include <rfb/ledStates.h>

#include <rdr/types.h>

#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wordexp.h>

using namespace rfb;

static LogWriter slog("VNCServerST");
LogWriter VNCServerST::connectionsLog("Connections");
EncCache VNCServerST::encCache;

//
// -=- VNCServerST Implementation
//

static char kasmpasswdpath[4096];

// -=- Constructors/Destructor

static void mixedPercentages() {
  slog.error("Mixing percentages and absolute values in DLP_Region is not allowed");
  exit(1);
}

static void parseRegionPart(const bool percents, rdr::U16 &pcdest, int &dest,
                            char **inptr) {
  char *nextptr, *ptr;
  ptr = *inptr;
  int val = strtol(ptr, &nextptr, 10);
  if (!*ptr || ptr == nextptr) {
    slog.error("Invalid value for DLP_Region");
    exit(1);
  }
  ptr = nextptr;
  if (*ptr == '%') {
    if (!percents)
      mixedPercentages();
    pcdest = val;

    if (val < 0 || val > 100) {
      slog.error("Percent must be 0-100");
      exit(1);
    }

    ptr++;
  } else if (percents) {
    mixedPercentages();
  }
  dest = val;

  for (; *ptr && *ptr == ','; ptr++);

  *inptr = ptr;
}

VNCServerST::VNCServerST(const char* name_, SDesktop* desktop_)
  : blHosts(&blacklist), desktop(desktop_), desktopStarted(false),
    blockCounter(0), pb(0), blackedpb(0), ledState(ledUnknown),
    name(strDup(name_)), pointerClient(0), clipboardClient(0),
    comparer(0), cursor(new Cursor(0, 0, Point(), NULL)),
    renderedCursorInvalid(false),
    queryConnectionHandler(0), keyRemapper(&KeyRemapper::defInstance),
    lastConnectionTime(0), disableclients(false),
    frameTimer(this), apimessager(NULL)
{
  lastUserInputTime = lastDisconnectTime = time(0);
  slog.debug("creating single-threaded server %s", name.buf);

  DLPRegion.enabled = DLPRegion.percents = false;

  if (Server::DLP_Region[0]) {
    unsigned len = strlen(Server::DLP_Region);
    unsigned i;
    unsigned commas = 0;
    int val;
    char *ptr, *nextptr;

    for (i = 0; i < len; i++) {
      if (Server::DLP_Region[i] == ',')
        commas++;
    }

    if (commas != 3) {
      slog.error("DLP_Region must contain four values");
      exit(1);
    }

    ptr = (char *) (const char *) Server::DLP_Region;

    val = strtol(ptr, &nextptr, 10);
    if (!*ptr || ptr == nextptr) {
      slog.error("Invalid value for DLP_Region");
      exit(1);
    }
    ptr = nextptr;
    if (*ptr == '%') {
      DLPRegion.percents = true;
      DLPRegion.pcx1 = val;
      ptr++;
    }
    DLPRegion.x1 = val;

    for (; *ptr && *ptr == ','; ptr++);

    parseRegionPart(DLPRegion.percents, DLPRegion.pcy1, DLPRegion.y1,
                    &ptr);
    parseRegionPart(DLPRegion.percents, DLPRegion.pcx2, DLPRegion.x2,
                    &ptr);
    parseRegionPart(DLPRegion.percents, DLPRegion.pcy2, DLPRegion.y2,
                    &ptr);

    // Validity checks
    if (!DLPRegion.percents) {
      if (DLPRegion.x1 > 0 && DLPRegion.x2 > 0 && DLPRegion.x2 <= DLPRegion.x1) {
        slog.error("DLP_Region x2 must be > x1");
        exit(1);
      }
      if (DLPRegion.y1 > 0 && DLPRegion.y2 > 0 && DLPRegion.y2 <= DLPRegion.y1) {
        slog.error("DLP_Region y2 must be > y1");
        exit(1);
      }
    }

    DLPRegion.enabled = 1;
  }

  kasmpasswdpath[0] = '\0';
  wordexp_t wexp;
  if (!wordexp(rfb::Server::kasmPasswordFile, &wexp, WRDE_NOCMD))
    strncpy(kasmpasswdpath, wexp.we_wordv[0], 4096);
  kasmpasswdpath[4095] = '\0';
  wordfree(&wexp);

  if (kasmpasswdpath[0] && access(kasmpasswdpath, R_OK) == 0) {
    // Set up a watch on the password file
    inotifyfd = inotify_init();
    if (inotifyfd < 0)
      slog.error("Failed to init inotify");

    int flags = fcntl(inotifyfd, F_GETFL, 0);
    fcntl(inotifyfd, F_SETFL, flags | O_NONBLOCK);

    if (inotify_add_watch(inotifyfd, kasmpasswdpath, IN_CLOSE_WRITE | IN_DELETE_SELF) < 0)
      slog.error("Failed to set watch");
  }
}

VNCServerST::~VNCServerST()
{
  slog.debug("shutting down server %s", name.buf);

  // Close any active clients, with appropriate logging & cleanup
  closeClients("Server shutdown");

  // Stop trying to render things
  stopFrameClock();

  // Delete all the clients, and their sockets, and any closing sockets
  //   NB: Deleting a client implicitly removes it from the clients list
  while (!clients.empty()) {
    delete clients.front();
  }

  // Stop the desktop object if active, *only* after deleting all clients!
  stopDesktop();

  if (comparer)
    comparer->logStats();
  delete comparer;

  delete cursor;
}


// SocketServer methods

void VNCServerST::addSocket(network::Socket* sock, bool outgoing)
{
  // - Check the connection isn't black-marked
  // *** do this in getSecurity instead?
  CharArray address(sock->getPeerAddress());
  if (blHosts->isBlackmarked(address.buf)) {
    connectionsLog.error("blacklisted: %s", address.buf);
    try {
      SConnection::writeConnFailedFromScratch("Too many security failures",
                                              &sock->outStream());
    } catch (rdr::Exception&) {
    }
    sock->shutdown();
    closingSockets.push_back(sock);
    return;
  }

  if (clients.empty()) {
    lastConnectionTime = time(0);
  }

  VNCSConnectionST* client = new VNCSConnectionST(this, sock, outgoing);
  client->init();
}

void VNCServerST::removeSocket(network::Socket* sock) {
  // - If the socket has resources allocated to it, delete them
  std::list<VNCSConnectionST*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      // - Delete the per-Socket resources
      delete *ci;

      // - Check that the desktop object is still required
      if (authClientCount() == 0)
        stopDesktop();

      if (comparer)
        comparer->logStats();

      return;
    }
  }

  // - If the Socket has no resources, it may have been a closingSocket
  closingSockets.remove(sock);
}

void VNCServerST::processSocketReadEvent(network::Socket* sock)
{
  // - Find the appropriate VNCSConnectionST and process the event
  std::list<VNCSConnectionST*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      (*ci)->processMessages();
      return;
    }
  }
  throw rdr::Exception("invalid Socket in VNCServerST");
}

void VNCServerST::processSocketWriteEvent(network::Socket* sock)
{
  // - Find the appropriate VNCSConnectionST and process the event
  std::list<VNCSConnectionST*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      (*ci)->flushSocket();
      return;
    }
  }
  throw rdr::Exception("invalid Socket in VNCServerST");
}

int VNCServerST::checkTimeouts()
{
  int timeout = 0;
  std::list<VNCSConnectionST*>::iterator ci, ci_next;

  soonestTimeout(&timeout, Timer::checkTimeouts());

  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    soonestTimeout(&timeout, (*ci)->checkIdleTimeout());
  }

  int timeLeft;
  time_t now = time(0);

  // Check MaxDisconnectionTime 
  if (rfb::Server::maxDisconnectionTime && clients.empty()) {
    if (now < lastDisconnectTime) {
      // Someone must have set the time backwards. 
      slog.info("Time has gone backwards - resetting lastDisconnectTime");
      lastDisconnectTime = now;
    }
    timeLeft = lastDisconnectTime + rfb::Server::maxDisconnectionTime - now;
    if (timeLeft < -60) {
      // Someone must have set the time forwards.
      slog.info("Time has gone forwards - resetting lastDisconnectTime");
      lastDisconnectTime = now;
      timeLeft = rfb::Server::maxDisconnectionTime;
    }
    if (timeLeft <= 0) { 
      slog.info("MaxDisconnectionTime reached, exiting");
      exit(0);
    }
    soonestTimeout(&timeout, timeLeft * 1000);
  }

  // Check MaxConnectionTime 
  if (rfb::Server::maxConnectionTime && lastConnectionTime && !clients.empty()) {
    if (now < lastConnectionTime) {
      // Someone must have set the time backwards. 
      slog.info("Time has gone backwards - resetting lastConnectionTime");
      lastConnectionTime = now;
    }
    timeLeft = lastConnectionTime + rfb::Server::maxConnectionTime - now;
    if (timeLeft < -60) {
      // Someone must have set the time forwards.
      slog.info("Time has gone forwards - resetting lastConnectionTime");
      lastConnectionTime = now;
      timeLeft = rfb::Server::maxConnectionTime;
    }
    if (timeLeft <= 0) {
      slog.info("MaxConnectionTime reached, exiting");
      exit(0);
    }
    soonestTimeout(&timeout, timeLeft * 1000);
  }

  
  // Check MaxIdleTime 
  if (rfb::Server::maxIdleTime) {
    if (now < lastUserInputTime) {
      // Someone must have set the time backwards. 
      slog.info("Time has gone backwards - resetting lastUserInputTime");
      lastUserInputTime = now;
    }
    timeLeft = lastUserInputTime + rfb::Server::maxIdleTime - now;
    if (timeLeft < -60) {
      // Someone must have set the time forwards.
      slog.info("Time has gone forwards - resetting lastUserInputTime");
      lastUserInputTime = now;
      timeLeft = rfb::Server::maxIdleTime;
    }
    if (timeLeft <= 0) {
      slog.info("MaxIdleTime reached, exiting");
      exit(0);
    }
    soonestTimeout(&timeout, timeLeft * 1000);
  }
  
  return timeout;
}


// VNCServer methods

void VNCServerST::blockUpdates()
{
  blockCounter++;

  stopFrameClock();
}

void VNCServerST::unblockUpdates()
{
  assert(blockCounter > 0);

  blockCounter--;

  // Restart the frame clock if we have updates
  if (blockCounter == 0) {
    if (!comparer->is_empty())
      startFrameClock();
  }
}

void VNCServerST::setPixelBuffer(PixelBuffer* pb_, const ScreenSet& layout)
{
  if (comparer)
    comparer->logStats();

  pb = pb_;
  delete comparer;
  comparer = 0;

  screenLayout = layout;

  if (!pb) {
    screenLayout = ScreenSet();

    if (desktopStarted)
      throw Exception("setPixelBuffer: null PixelBuffer when desktopStarted?");

    return;
  }

  // Assume the framebuffer contents wasn't saved and reset everything
  // that tracks its contents
  comparer = new ComparingUpdateTracker(pb);
  renderedCursorInvalid = true;
  add_changed(pb->getRect());

  // Make sure that we have at least one screen
  if (screenLayout.num_screens() == 0)
    screenLayout.add_screen(Screen(0, 0, 0, pb->width(), pb->height(), 0));

  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->pixelBufferChange();
    // Since the new pixel buffer means an ExtendedDesktopSize needs to
    // be sent anyway, we don't need to call screenLayoutChange.
  }
}

void VNCServerST::setPixelBuffer(PixelBuffer* pb_)
{
  ScreenSet layout = screenLayout;

  // Check that the screen layout is still valid
  if (pb_ && !layout.validate(pb_->width(), pb_->height())) {
    Rect fbRect;
    ScreenSet::iterator iter, iter_next;

    fbRect.setXYWH(0, 0, pb_->width(), pb_->height());

    for (iter = layout.begin();iter != layout.end();iter = iter_next) {
      iter_next = iter; ++iter_next;
      if (iter->dimensions.enclosed_by(fbRect))
          continue;
      iter->dimensions = iter->dimensions.intersect(fbRect);
      if (iter->dimensions.is_empty()) {
        slog.info("Removing screen %d (%x) as it is completely outside the new framebuffer",
                  (int)iter->id, (unsigned)iter->id);
        layout.remove_screen(iter->id);
      }
    }
  }

  setPixelBuffer(pb_, layout);
}

void VNCServerST::setScreenLayout(const ScreenSet& layout)
{
  if (!pb)
    throw Exception("setScreenLayout: new screen layout without a PixelBuffer");
  if (!layout.validate(pb->width(), pb->height()))
    throw Exception("setScreenLayout: invalid screen layout");

  screenLayout = layout;

  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->screenLayoutChangeOrClose(reasonServer);
  }
}

void VNCServerST::requestClipboard()
{
  if (clipboardClient == NULL)
    return;

  clipboardClient->requestClipboard();
}

void VNCServerST::announceClipboard(bool available)
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;

  if (available)
    clipboardClient = NULL;

  clipboardRequestors.clear();

  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->announceClipboard(available);
  }
}

void VNCServerST::sendClipboardData(const char* data)
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;

  if (strchr(data, '\r') != NULL)
    throw Exception("Invalid carriage return in clipboard data");

  for (ci = clipboardRequestors.begin();
       ci != clipboardRequestors.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->sendClipboardDataOrClose(data);
  }

  clipboardRequestors.clear();
}

void VNCServerST::bell()
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->bellOrClose();
  }
}

void VNCServerST::setName(const char* name_)
{
  name.replaceBuf(strDup(name_));
  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->setDesktopNameOrClose(name_);
  }
}

void VNCServerST::add_changed(const Region& region)
{
  if (comparer == NULL)
    return;

  comparer->add_changed(region);
  startFrameClock();
}

void VNCServerST::add_copied(const Region& dest, const Point& delta)
{
  if (comparer == NULL)
    return;

  comparer->add_copied(dest, delta);
  startFrameClock();
}

void VNCServerST::setCursor(int width, int height, const Point& newHotspot,
                            const rdr::U8* data)
{
  delete cursor;
  cursor = new Cursor(width, height, newHotspot, data);
  cursor->crop();

  renderedCursorInvalid = true;

  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->renderedCursorChange();
    (*ci)->setCursorOrClose();
  }
}

void VNCServerST::setCursorPos(const Point& pos, bool warped)
{
  if (!cursorPos.equals(pos)) {
    cursorPos = pos;
    renderedCursorInvalid = true;
    std::list<VNCSConnectionST*>::iterator ci;
    for (ci = clients.begin(); ci != clients.end(); ci++) {
      (*ci)->renderedCursorChange();
      if (warped)
        (*ci)->cursorPositionChange();
    }
  }
}

void VNCServerST::setLEDState(unsigned int state)
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;

  if (state == ledState)
    return;

  ledState = state;

  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;
    (*ci)->setLEDStateOrClose(state);
  }
}

// Other public methods

void VNCServerST::approveConnection(network::Socket* sock, bool accept,
                                    const char* reason)
{
  std::list<VNCSConnectionST*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock) {
      (*ci)->approveConnectionOrClose(accept, reason);
      return;
    }
  }
}

void VNCServerST::closeClients(const char* reason, network::Socket* except)
{
  std::list<VNCSConnectionST*>::iterator i, next_i;
  for (i=clients.begin(); i!=clients.end(); i=next_i) {
    next_i = i; next_i++;
    if ((*i)->getSock() != except)
      (*i)->close(reason);
  }
}

void VNCServerST::getSockets(std::list<network::Socket*>* sockets)
{
  sockets->clear();
  std::list<VNCSConnectionST*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    sockets->push_back((*ci)->getSock());
  }
  std::list<network::Socket*>::iterator si;
  for (si = closingSockets.begin(); si != closingSockets.end(); si++) {
    sockets->push_back(*si);
  }
}

SConnection* VNCServerST::getSConnection(network::Socket* sock) {
  std::list<VNCSConnectionST*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->getSock() == sock)
      return *ci;
  }
  return 0;
}

bool VNCServerST::handleTimeout(Timer* t)
{
  if (t == &frameTimer) {
    // We keep running until we go a full interval without any updates
    if (comparer->is_empty())
      return false;

    writeUpdate();

    // If this is the first iteration then we need to adjust the timeout
    if (frameTimer.getTimeoutMs() != 1000/rfb::Server::frameRate) {
      frameTimer.start(1000/rfb::Server::frameRate);
      return false;
    }

    return true;
  }

  return false;
}

// -=- Internal methods

void VNCServerST::startDesktop()
{
  if (!desktopStarted) {
    slog.debug("starting desktop");
    desktop->start(this);
    if (!pb)
      throw Exception("SDesktop::start() did not set a valid PixelBuffer");
    desktopStarted = true;
    // The tracker might have accumulated changes whilst we were
    // stopped, so flush those out
    if (!comparer->is_empty())
      writeUpdate();
  }
}

void VNCServerST::stopDesktop()
{
  if (desktopStarted) {
    slog.debug("stopping desktop");
    desktopStarted = false;
    desktop->stop();
    stopFrameClock();
  }
}

int VNCServerST::authClientCount() {
  int count = 0;
  std::list<VNCSConnectionST*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++) {
    if ((*ci)->authenticated())
      count++;
  }
  return count;
}

inline bool VNCServerST::needRenderedCursor()
{
  std::list<VNCSConnectionST*>::iterator ci;
  for (ci = clients.begin(); ci != clients.end(); ci++)
    if ((*ci)->needRenderedCursor()) return true;
  return false;
}

void VNCServerST::startFrameClock()
{
  if (frameTimer.isStarted())
    return;
  if (blockCounter > 0)
    return;
  if (!desktopStarted)
    return;

  // The first iteration will be just half a frame as we get a very
  // unstable update rate if we happen to be perfectly in sync with
  // the application's update rate
  frameTimer.start(1000/rfb::Server::frameRate/2);
}

void VNCServerST::stopFrameClock()
{
  frameTimer.stop();
}

int VNCServerST::msToNextUpdate()
{
  // FIXME: If the application is updating slower than frameRate then
  //        we could allow the clients more time here

  if (!frameTimer.isStarted())
    return 1000/rfb::Server::frameRate/2;
  else
    return frameTimer.getRemainingMs();
}

static void checkAPIMessages(network::GetAPIMessager *apimessager)
{
  if (pthread_mutex_lock(&apimessager->userMutex))
    return;

  const unsigned num = apimessager->actionQueue.size();
  unsigned i;
  for (i = 0; i < num; i++) {
    slog.info("Main thread processing user API request %u/%u", i + 1, num);

    const network::GetAPIMessager::action_data &act = apimessager->actionQueue[i];
    struct kasmpasswd_t *set = NULL;
    unsigned s;
    bool found;

    switch (act.action) {
      case network::GetAPIMessager::USER_REMOVE:
        set = readkasmpasswd(kasmpasswdpath);
        found = false;
        for (s = 0; s < set->num; s++) {
          if (!strcmp(set->entries[s].user, act.data.user)) {
            set->entries[s].user[0] = '\0';
            found = true;
            break;
          }
        }

        if (found) {
          writekasmpasswd(kasmpasswdpath, set);
          slog.info("User %s removed", act.data.user);
        } else {
          slog.error("Tried to remove nonexistent user %s", act.data.user);
        }
      break;
      case network::GetAPIMessager::USER_GIVE_CONTROL:
        set = readkasmpasswd(kasmpasswdpath);
        found = false;
        for (s = 0; s < set->num; s++) {
          if (!strcmp(set->entries[s].user, act.data.user)) {
            set->entries[s].write = 1;
            found = true;
          } else {
            set->entries[s].write = 0;
          }
        }

        if (found) {
          writekasmpasswd(kasmpasswdpath, set);
          slog.info("User %s given control", act.data.user);
        } else {
          slog.error("Tried to give control to nonexistent user %s", act.data.user);
        }
      break;
    }

    if (set) {
      free(set->entries);
      free(set);
    }
  }

  apimessager->actionQueue.clear();
  pthread_mutex_unlock(&apimessager->userMutex);
}

void VNCServerST::translateDLPRegion(rdr::U16 &x1, rdr::U16 &y1, rdr::U16 &x2, rdr::U16 &y2) const
{
  if (DLPRegion.percents) {
    x1 = DLPRegion.pcx1 ? DLPRegion.pcx1 * pb->getRect().width() / 100 : 0;
    y1 = DLPRegion.pcy1 ? DLPRegion.pcy1 * pb->getRect().height() / 100 : 0;
    x2 = DLPRegion.pcx2 ? (100 - DLPRegion.pcx2) * pb->getRect().width() / 100 : pb->getRect().width();
    y2 = DLPRegion.pcy2 ? (100 - DLPRegion.pcy2) * pb->getRect().height() / 100 : pb->getRect().height();
  } else {
    x1 = abs(DLPRegion.x1);
    y1 = abs(DLPRegion.y1);
    x2 = pb->getRect().width();
    y2 = pb->getRect().height();

    if (DLPRegion.x2 < 0)
      x2 += DLPRegion.x2;
    else if (DLPRegion.x2 > 0)
      x2 = DLPRegion.x2;

    if (DLPRegion.y2 < 0)
      y2 += DLPRegion.y2;
    else if (DLPRegion.y2 > 0)
      y2 = DLPRegion.y2;
  }

  if (y2 > pb->getRect().height())
    y2 = pb->getRect().height() - 1;
  if (x2 > pb->getRect().width())
    x2 = pb->getRect().width() - 1;

  //slog.info("DLP_Region vals %u,%u %u,%u", x1, y1, x2, y2);
}

void VNCServerST::blackOut()
{
  // Compute the region, since the resolution may have changed
  rdr::U16 x1, y1, x2, y2;

  translateDLPRegion(x1, y1, x2, y2);

  if (blackedpb)
    delete blackedpb;
  blackedpb = new ManagedPixelBuffer(pb->getPF(), pb->getRect().width(), pb->getRect().height());

  int stride;
  const rdr::U8 *src = pb->getBuffer(pb->getRect(), &stride);
  rdr::U8 *data = blackedpb->getBufferRW(pb->getRect(), &stride);
  stride *= 4;

  memcpy(data, src, stride * pb->getRect().height());

  rdr::U16 y;
  const rdr::U16 w = pb->getRect().width();
  const rdr::U16 h = pb->getRect().height();
  for (y = 0; y < h; y++) {
    if (y < y1 || y > y2) {
      memset(data, 0, stride);
    } else {
      if (x1)
        memset(data, 0, x1 * 4);
      if (x2)
        memset(&data[x2 * 4], 0, (w - x2) * 4);
    }

    data += stride;
  }
}

// writeUpdate() is called on a regular interval in order to see what
// updates are pending and propagates them to the update tracker for
// each client. It uses the ComparingUpdateTracker's compare() method
// to filter out areas of the screen which haven't actually changed. It
// also checks the state of the (server-side) rendered cursor, if
// necessary rendering it again with the correct background.

void VNCServerST::writeUpdate()
{
  UpdateInfo ui;
  Region toCheck;

  std::list<VNCSConnectionST*>::iterator ci, ci_next;

  assert(blockCounter == 0);
  assert(desktopStarted);

  if (DLPRegion.enabled) {
    comparer->enable_copyrect(false);
    blackOut();
  }

  comparer->getUpdateInfo(&ui, pb->getRect());
  toCheck = ui.changed.union_(ui.copied);

  Region cursorReg;
  if (needRenderedCursor()) {
    Rect clippedCursorRect = Rect(0, 0, cursor->width(), cursor->height())
                             .translate(cursorPos.subtract(cursor->hotspot()))
                             .intersect(pb->getRect());

    if (!toCheck.intersect(clippedCursorRect).is_empty())
      renderedCursorInvalid = true;
    cursorReg = clippedCursorRect;
  }

  pb->grabRegion(toCheck);

  if (getComparerState())
    comparer->enable();
  else
    comparer->disable();

  // Skip scroll detection if the client is slow, and didn't get the previous one yet
  if (comparer->compare(clients.size() == 1 && (*clients.begin())->has_copypassed(),
                        cursorReg))
    comparer->getUpdateInfo(&ui, pb->getRect());

  comparer->clear();

  encCache.clear();
  encCache.enabled = clients.size() > 1;

  // Check if the password file was updated
  bool permcheck = false;
  if (inotifyfd >= 0) {
    char buf[256];
    int ret = read(inotifyfd, buf, 256);
    int pos = 0;
    while (ret > 0) {
      const struct inotify_event * const ev = (struct inotify_event *) &buf[pos];

      if (ev->mask & IN_IGNORED) {
        // file was deleted, set new watch
        if (inotify_add_watch(inotifyfd, kasmpasswdpath, IN_CLOSE_WRITE | IN_DELETE_SELF) < 0)
          slog.error("Failed to set watch");
      }

      permcheck = true;

      ret -= sizeof(struct inotify_event) - ev->len;
      pos += sizeof(struct inotify_event) - ev->len;
    }
  }

  if (apimessager) {
    apimessager->mainUpdateScreen(pb);

    checkAPIMessages(apimessager);
  }

  for (ci = clients.begin(); ci != clients.end(); ci = ci_next) {
    ci_next = ci; ci_next++;

    if (permcheck)
      (*ci)->recheckPerms();

    (*ci)->add_copied(ui.copied, ui.copy_delta);
    (*ci)->add_copypassed(ui.copypassed);
    (*ci)->add_changed(ui.changed);
    (*ci)->writeFramebufferUpdateOrClose();
  }
}

// checkUpdate() is called by clients to see if it is safe to read from
// the framebuffer at this time.

Region VNCServerST::getPendingRegion()
{
  UpdateInfo ui;

  // Block clients as the frame buffer cannot be safely accessed
  if (blockCounter > 0)
    return pb->getRect();

  // Block client from updating if there are pending updates
  if (comparer->is_empty())
    return Region();

  comparer->getUpdateInfo(&ui, pb->getRect());

  return ui.changed.union_(ui.copied);
}

const RenderedCursor* VNCServerST::getRenderedCursor()
{
  if (renderedCursorInvalid) {
    renderedCursor.update(pb, cursor, cursorPos);
    renderedCursorInvalid = false;
  }

  return &renderedCursor;
}

void VNCServerST::getConnInfo(ListConnInfo * listConn)
{
  listConn->Clear();
  listConn->setDisable(getDisable());
  if (clients.empty())
    return;
  std::list<VNCSConnectionST*>::iterator i;
  for (i = clients.begin(); i != clients.end(); i++)
    listConn->addInfo((void*)(*i), (*i)->getSock()->getPeerAddress(),
                      (*i)->getStartTime(), (*i)->getStatus());
}

void VNCServerST::setConnStatus(ListConnInfo* listConn)
{
  setDisable(listConn->getDisable());
  if (listConn->Empty() || clients.empty()) return;
  for (listConn->iBegin(); !listConn->iEnd(); listConn->iNext()) {
    VNCSConnectionST* conn = (VNCSConnectionST*)listConn->iGetConn();
    std::list<VNCSConnectionST*>::iterator i;
    for (i = clients.begin(); i != clients.end(); i++) {
      if ((*i) == conn) {
        int status = listConn->iGetStatus();
        if (status == 3) {
          (*i)->close(0);
        } else {
          (*i)->setStatus(status);
        }
        break;
      }
    }
  }
}

void VNCServerST::notifyScreenLayoutChange(VNCSConnectionST* requester)
{
  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    if ((*ci) == requester)
      continue;
    (*ci)->screenLayoutChangeOrClose(reasonOtherClient);
  }
}

bool VNCServerST::getComparerState()
{
  if (rfb::Server::compareFB == 0)
    return false;
  if (rfb::Server::compareFB != 2)
    return true;

  std::list<VNCSConnectionST*>::iterator ci, ci_next;
  for (ci=clients.begin();ci!=clients.end();ci=ci_next) {
    ci_next = ci; ci_next++;
    if ((*ci)->getComparerState())
      return true;
  }
  return false;
}

void VNCServerST::handleClipboardRequest(VNCSConnectionST* client)
{
  clipboardRequestors.push_back(client);
  if (clipboardRequestors.size() == 1)
    desktop->handleClipboardRequest();
}

void VNCServerST::handleClipboardAnnounce(VNCSConnectionST* client,
                                          bool available)
{
  if (available)
    clipboardClient = client;
  else {
    if (client != clipboardClient)
      return;
    clipboardClient = NULL;
  }
  desktop->handleClipboardAnnounce(available);
}

void VNCServerST::handleClipboardData(VNCSConnectionST* client,
                                      const char* data, int len)
{
  if (client != clipboardClient)
    return;
  desktop->handleClipboardData(data, len);
}


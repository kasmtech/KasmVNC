/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2016 Pierre Ossman for Cendio AB
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
// VNCSConnectionST is our derived class of SConnection for VNCServerST - there
// is one for each connected client.  We think of VNCSConnectionST as part of
// the VNCServerST implementation, so its methods are allowed full access to
// members of VNCServerST.
//

#ifndef __RFB_VNCSCONNECTIONST_H__
#define __RFB_VNCSCONNECTIONST_H__

#include <map>

#include <rfb/Congestion.h>
#include <rfb/EncodeManager.h>
#include <rfb/SConnection.h>
#include <rfb/Timer.h>

namespace rfb {
  class VNCServerST;

  class VNCSConnectionST : public SConnection,
                           public Timer::Callback {
  public:
    VNCSConnectionST(VNCServerST* server_, network::Socket* s, bool reverse);
    virtual ~VNCSConnectionST();

    // Methods called from VNCServerST.  None of these methods ever knowingly
    // throw an exception.

    // Unless otherwise stated, the SConnectionST may not be valid after any of
    // these methods are called, since they catch exceptions and may have
    // called close() which deletes the object.

    // init() must be called to initialise the protocol.  If it fails it
    // returns false, and close() will have been called.
    bool init();

    // close() shuts down the socket to the client and deletes the
    // SConnectionST object.
    void close(const char* reason);

    // processMessages() processes incoming messages from the client, invoking
    // various callbacks as a result.  It continues to process messages until
    // reading might block.  shutdown() will be called on the connection's
    // Socket if an error occurs, via the close() call.
    void processMessages();

    // flushSocket() pushes any unwritten data on to the network.
    void flushSocket();

    // Called when the underlying pixelbuffer is resized or replaced.
    void pixelBufferChange();

    // Wrappers to make these methods "safe" for VNCServerST.
    void writeFramebufferUpdateOrClose();
    void screenLayoutChangeOrClose(rdr::U16 reason);
    void setCursorOrClose();
    void bellOrClose();
    void setDesktopNameOrClose(const char *name);
    void setLEDStateOrClose(unsigned int state);
    void requestClipboardOrClose();
    void announceClipboardOrClose(bool available);
    void sendClipboardDataOrClose(const char* data);

    // checkIdleTimeout() returns the number of milliseconds left until the
    // idle timeout expires.  If it has expired, the connection is closed and
    // zero is returned.  Zero is also returned if there is no idle timeout.
    int checkIdleTimeout();

    // The following methods never throw exceptions nor do they ever delete the
    // SConnectionST object.

    // getComparerState() returns if this client would like the framebuffer
    // comparer to be enabled.
    bool getComparerState();

    // renderedCursorChange() is called whenever the server-side rendered
    // cursor changes shape or position.  It ensures that the next update will
    // clean up the old rendered cursor and if necessary draw the new rendered
    // cursor.
    void renderedCursorChange();

    // cursorPositionChange() is called whenever the cursor has changed position by
    // the server.  If the client supports being informed about these changes then
    // it will arrange for the new cursor position to be sent to the client.
    void cursorPositionChange();

    // needRenderedCursor() returns true if this client needs the server-side
    // rendered cursor.  This may be because it does not support local cursor
    // or because the current cursor position has not been set by this client.
    bool needRenderedCursor();

    void recheckPerms() {
        needsPermCheck = true;
    }

    network::Socket* getSock() { return sock; }
    void add_changed(const Region& region) { updates.add_changed(region); }
    void add_changed_all() { updates.add_changed(server->pb->getRect()); }
    void add_copied(const Region& dest, const Point& delta) {
      updates.add_copied(dest, delta);
    }
    void add_copypassed(const std::vector<CopyPassRect> &in) {
      // If we're adding to a non-empty list, it means something
      // changed, and we have to convert the new list into changes.
      //
      // The new change might be a scroll, and just adding would break
      // the order!
      // (copies 1f, changes 1f, copies 2f, changes 2f -> copies, changes)
      //
      // However, it shouldn't happen, as we changed the caller to save
      // cpu on those frames. No point calculating scroll copies that wouldn't
      // be used. It should only happen if there's multiple clients, and one is
      // slow.

      if (copypassed.size()) {
        Region everything;
        for (std::vector<CopyPassRect>::const_iterator it = in.begin();
             it != in.end(); it++) {
          everything.assign_union(it->rect);
        }
        add_changed(everything);
        return;
      }

      copypassed = in;
    }

    bool has_copypassed() const {
      return copypassed.size() != 0;
    }

    const char* getPeerEndpoint() const {return peerEndpoint.buf;}

    // approveConnectionOrClose() is called some time after
    // VNCServerST::queryConnection() has returned with PENDING to accept or
    // reject the connection.  The accept argument should be true for
    // acceptance, or false for rejection, in which case a string reason may
    // also be given.

    void approveConnectionOrClose(bool accept, const char* reason);

    char* getStartTime();

    void setStatus(int status);
    int getStatus();

  private:
    // SConnection callbacks

    // These methods are invoked as callbacks from processMsg().  Note that
    // none of these methods should call any of the above methods which may
    // delete the SConnectionST object.

    virtual void authSuccess();
    virtual void queryConnection(const char* userName);
    virtual void clientInit(bool shared);
    virtual void setPixelFormat(const PixelFormat& pf);
    virtual void pointerEvent(const Point& pos, int buttonMask, const bool skipClick, const bool skipRelease);
    virtual void keyEvent(rdr::U32 keysym, rdr::U32 keycode, bool down);
    virtual void framebufferUpdateRequest(const Rect& r, bool incremental);
    virtual void setDesktopSize(int fb_width, int fb_height,
                                const ScreenSet& layout);
    virtual void fence(rdr::U32 flags, unsigned len, const char data[]);
    virtual void enableContinuousUpdates(bool enable,
                                         int x, int y, int w, int h);
    virtual void handleClipboardRequest();
    virtual void handleClipboardAnnounce(bool available);
    virtual void handleClipboardData(const char* data, int len);
    virtual void supportsLocalCursor();
    virtual void supportsFence();
    virtual void supportsContinuousUpdates();
    virtual void supportsLEDState();

    virtual void sendStats();
    virtual bool canChangeKasmSettings() const {
        return (accessRights & (AccessPtrEvents | AccessKeyEvents)) ==
               (AccessPtrEvents | AccessKeyEvents);
    }

    // setAccessRights() allows a security package to limit the access rights
    // of a VNCSConnectioST to the server.  These access rights are applied
    // such that the actual rights granted are the minimum of the server's
    // default access settings and the connection's access settings.
    virtual void setAccessRights(AccessRights ar) {
        accessRights = ar;

        bool write, owner;
        if (!getPerms(write, owner) || !write)
            accessRights &= ~WRITER_PERMS;
        needsPermCheck = false;
    }

    // Timer callbacks
    virtual bool handleTimeout(Timer* t);

    // Internal methods

    bool isShiftPressed();

    bool getPerms(bool &write, bool &owner) const;

    // Congestion control
    void writeRTTPing();
    bool isCongested();

    // writeFramebufferUpdate() attempts to write a framebuffer update to the
    // client.

    void writeFramebufferUpdate();
    void writeNoDataUpdate();
    void writeDataUpdate();

    void screenLayoutChange(rdr::U16 reason);
    void setCursor();
    void setCursorPos();
    void setDesktopName(const char *name);
    void setLEDState(unsigned int state);
    void setSocketTimeouts();

    network::Socket* sock;
    CharArray peerEndpoint;
    bool reverseConnection;

    bool inProcessMessages;

    bool pendingSyncFence, syncFence;
    rdr::U32 fenceFlags;
    unsigned fenceDataLen;
    char *fenceData;

    Congestion congestion;
    Timer congestionTimer;
    Timer losslessTimer;
    Timer kbdLogTimer;

    VNCServerST* server;
    SimpleUpdateTracker updates;
    Region requested;
    bool updateRenderedCursor, removeRenderedCursor;
    Region damagedCursorRegion;
    bool continuousUpdates;
    Region cuRegion;
    EncodeManager encodeManager;

    std::map<rdr::U32, rdr::U32> pressedKeys;

    enum {
        BS_CPU_CLOSE,
        BS_CPU_SLOW,
        BS_NET_SLOW,
        BS_FRAME,

        BS_NUM
    };
    std::list<struct timeval> bstats[BS_NUM]; // Bottleneck stats
    rdr::U64 bstats_total[BS_NUM];
    struct timeval connStart;

    char user[32];
    char kasmpasswdpath[4096];
    bool needsPermCheck;

    time_t lastEventTime;
    time_t pointerEventTime;
    Point pointerEventPos;
    bool clientHasCursor;
    struct timeval lastRealUpdate;
    struct timeval lastClipboardOp;
    struct timeval lastKeyEvent;

    AccessRights accessRights;

    CharArray closeReason;
    time_t startTime;

    std::vector<CopyPassRect> copypassed;
  };
}
#endif

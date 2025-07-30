/* Copyright (C) 2021 Kasm
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

#ifndef __NETWORK_GET_API_H__
#define __NETWORK_GET_API_H__

#include <kasmpasswd.h>
#include <pthread.h>
#include <network/GetAPIEnums.h>
#include <rfb/PixelBuffer.h>
#include <rfb/PixelFormat.h>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>
#include <mutex>

namespace network {

  class GetAPIMessager {
  public:
    GetAPIMessager(const char *passwdfile_);

    // from main thread
    void mainUpdateScreen(rfb::PixelBuffer *pb);
    void mainUpdateBottleneckStats(const char userid[], const char stats[]);
    void mainClearBottleneckStats(const char userid[]);
    void mainUpdateServerFrameStats(uint8_t changedPerc, uint32_t all,
                                    uint32_t jpeg, uint32_t webp, uint32_t analysis,
                                    uint32_t jpegarea, uint32_t webparea,
                                    uint16_t njpeg, uint16_t nwebp,
                                    uint16_t enc, uint16_t scale, uint16_t shot,
                                    uint16_t w, uint16_t h);
    void mainUpdateClientFrameStats(const char userid[], uint32_t render, uint32_t all,
                                    uint32_t ping);
    void mainUpdateUserInfo(const uint8_t ownerConn, const uint8_t numUsers);

    void mainUpdateSessionsInfo(std::string newSessionsInfo);

    // from network threads
    uint8_t *netGetScreenshot(uint16_t w, uint16_t h,
                              const uint8_t q, const bool dedup,
                              uint32_t &len, uint8_t *staging);
    uint8_t netAddUser(const char name[], const char pw[],
                       const bool read, const bool write, const bool owner);
    uint8_t netRemoveUser(const char name[]);
    uint8_t netUpdateUser(const char name[], const uint64_t mask,
                          const char password[],
                          const bool read, const bool write, const bool owner);
    uint8_t netAddOrUpdateUser(const struct kasmpasswd_entry_t *entry);
    void netGetUsers(const char **ptr);

    const std::string_view netGetSessions();
    void netGetBottleneckStats(char *buf, uint32_t len);
    void netGetFrameStats(char *buf, uint32_t len);
    void netResetFrameStatsCall();
    uint8_t netServerFrameStatsReady();
    void netUdpUpgrade(void *client, uint32_t ip);
    void netClearClipboard();

    enum USER_ACTION {
      NONE,
      WANT_FRAME_STATS_SERVERONLY,
      WANT_FRAME_STATS_ALL,
      WANT_FRAME_STATS_OWNER,
      WANT_FRAME_STATS_SPECIFIC,
      UDP_UPGRADE,
      CLEAR_CLIPBOARD,
    };

    uint8_t netRequestFrameStats(USER_ACTION what, const char *client);
    uint8_t netOwnerConnected();
    uint8_t netNumActiveUsers();
    uint8_t netGetClientFrameStatsNum();

    struct action_data {
      enum USER_ACTION action;
      union {
        kasmpasswd_entry_t data;
        struct {
          void *client;
          uint32_t ip;
        } udp;
      };
    };

    pthread_mutex_t userMutex;
    std::vector<action_data> actionQueue;

  private:
    const char *passwdfile;

    pthread_mutex_t screenMutex;
    rfb::ManagedPixelBuffer screenPb;
    uint16_t screenW, screenH;
    uint64_t screenHash;

    std::vector<uint8_t> cachedJpeg;
    uint16_t cachedW, cachedH;
    uint8_t cachedQ;

    std::map<std::string, std::string> bottleneckStats;
    pthread_mutex_t statMutex;

    struct clientFrameStats_t {
      uint32_t render;
      uint32_t all;
      uint32_t ping;
    };
    struct serverFrameStats_t {
      uint32_t all;
      uint32_t jpeg;
      uint32_t webp;
      uint32_t analysis;
      uint32_t jpegarea;
      uint32_t webparea;
      uint16_t njpeg;
      uint16_t nwebp;
      uint16_t enc;
      uint16_t scale;
      uint16_t shot;
      uint16_t w;
      uint16_t h;
      uint8_t changedPerc;

      uint8_t inprogress;
    };
    std::map<std::string, clientFrameStats_t> clientFrameStats;
    serverFrameStats_t serverFrameStats;
    pthread_mutex_t frameStatMutex;

    uint8_t ownerConnected;
    uint8_t activeUsers;
    pthread_mutex_t userInfoMutex;
    std::mutex sessionInfoMutex;
    std::string sessionsInfo;
  };

}

#endif // __NETWORK_GET_API_H__

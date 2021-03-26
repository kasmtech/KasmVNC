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
#include <rfb/PixelBuffer.h>
#include <rfb/PixelFormat.h>
#include <stdint.h>
#include <vector>

namespace network {

  class GetAPIMessager {
  public:
    GetAPIMessager(const char *passwdfile_);

    // from main thread
    void mainUpdateScreen(rfb::PixelBuffer *pb);

    // from network threads
    uint8_t *netGetScreenshot(uint16_t w, uint16_t h,
                              const uint8_t q, const bool dedup,
                              uint32_t &len, uint8_t *staging);
    uint8_t netAddUser(const char name[], const char pw[], const bool write);
    uint8_t netRemoveUser(const char name[]);
    uint8_t netGiveControlTo(const char name[]);

    enum USER_ACTION {
      //USER_ADD, - handled locally for interactivity
      USER_REMOVE,
      USER_GIVE_CONTROL,
    };

    struct action_data {
      enum USER_ACTION action;
      kasmpasswd_entry_t data;
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
  };

}

#endif // __NETWORK_GET_API_H__

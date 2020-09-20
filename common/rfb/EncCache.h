/* Copyright (C) 2000-2003 Constantin Kaplinsky.  All Rights Reserved.
 * Copyright (C) 2011 D. R. Commander.  All Rights Reserved.
 * Copyright 2014-2018 Pierre Ossman for Cendio AB
 * Copyright (C) 2019 Lauri Kasanen
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
#ifndef __RFB_ENCCACHE_H__
#define __RFB_ENCCACHE_H__

#include <map>

#include <rdr/types.h>

#include <stdint.h>
#include <stdlib.h>

namespace rfb {

  struct EncId {
    uint8_t type;
    uint16_t x, y, w, h;
    uint32_t len;

    bool operator <(const EncId &other) const {
      return type < other.type ||
             x < other.x ||
             y < other.y ||
             w < other.w ||
             h < other.h;
    }
  };

  class EncCache {
  public:
    EncCache();
    ~EncCache();

    void clear();
    void add(uint8_t type, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
             uint32_t len, const void *data);
    const void *get(uint8_t type, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                    uint32_t &len) const;

    bool enabled;

  protected:
    std::map<EncId, const void *> cache;
  };
}

#endif

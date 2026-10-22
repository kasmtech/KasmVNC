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
#include <rfb/EncCache.h>

using namespace rfb;

EncCache::EncCache() {
  enabled = false;
}

EncCache::~EncCache() {
    clear();
}

void EncCache::clear() {
    for (const auto &item : cache)
        free(item.second.data);
    cache.clear();
}

void EncCache::add(uint8_t type, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint32_t len, const void *data) {
    const EncId id{type, x, y, w, h};

    const auto [it, inserted] = cache.try_emplace(id, const_cast<void *>(data), len);
    if (inserted)
        return;

    Entry &entry = it->second;
    if (entry.data != data) {
        free(entry.data);
        entry.data = const_cast<void *>(data);
    }
    entry.len = len;
}

const void *EncCache::get(uint8_t type, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint32_t &len) const {
    EncId id{.type = type, .x = x, .y = y, .w = w, .h = h};

  const auto it = cache.find(id);
  if (it == cache.end())
    return nullptr;

  len = it->second.len;
  return it->second.data;
}

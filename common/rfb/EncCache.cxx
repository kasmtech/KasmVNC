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

#include <memory>

using namespace rfb;

EncCache::EncCache() {
  enabled = false;
}

EncCache::~EncCache() {
  clear();
}

void EncCache::clear() {
  std::map<EncId, Entry>::iterator it;
  for (it = cache.begin(); it != cache.end(); it++)
    free((void *) it->second.first);

  cache.clear();
}

void EncCache::add(uint8_t type, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint32_t len, const void *data) {

  EncId id;

  id.type = type;
  id.x = x;
  id.y = y;
  id.w = w;
  id.h = h;

  auto it = cache.find(id);
  if (it != cache.end()) {
    if (it->second.first != data)
      free(const_cast<void *>(it->second.first));
    it->second = Entry(data, len);
  } else {
    // Retain ownership if allocating the map node throws.
    std::unique_ptr<void, decltype(&free)> owned(const_cast<void *>(data), &free);
    cache.emplace(id, Entry(data, len));
    owned.release();
  }
}

const void *EncCache::get(uint8_t type, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          uint32_t &len) const {

  EncId id;

  id.type = type;
  id.x = x;
  id.y = y;
  id.w = w;
  id.h = h;

  std::map<EncId, Entry>::const_iterator it = cache.find(id);
  if (it == cache.end())
    return NULL;

  len = it->second.second;
  return it->second.first;
}

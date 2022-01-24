/* Copyright (C) 2022 Kasm
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

#ifndef __NETWORK_JSON_ESCAPE_H__
#define __NETWORK_JSON_ESCAPE_H__

#include <kasmpasswd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void JSON_escape(const char *in, char *out);
void JSON_unescape(const char *in, char *out);

struct kasmpasswd_t *parseJsonUsers(const char *data);

#ifdef __cplusplus
} // extern C
#endif

#endif

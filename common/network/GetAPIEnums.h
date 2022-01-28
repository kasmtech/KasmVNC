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

#ifndef __NETWORK_GET_API_ENUMS_H__
#define __NETWORK_GET_API_ENUMS_H__

// Enums that need accessibility from both C and C++.
enum USER_UPDATE_MASK {
	USER_UPDATE_WRITE_MASK = 1 << 0,
	USER_UPDATE_OWNER_MASK = 1 << 1,
	USER_UPDATE_PASSWORD_MASK = 1 << 2,
	USER_UPDATE_READ_MASK = 1 << 3,
};

#endif

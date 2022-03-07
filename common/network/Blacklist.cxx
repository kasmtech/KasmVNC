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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include <map>
#include <string>

#include <network/Blacklist.h>
#include <rfb/Blacklist.h>

static std::map<std::string, unsigned> hits;
static std::map<std::string, time_t> blacklist;

static pthread_mutex_t hitmutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t blmutex = PTHREAD_MUTEX_INITIALIZER;

unsigned char bl_isBlacklisted(const char *addr) {
	const unsigned char count = blacklist.count(addr);
	if (!count)
		return 0;

	const time_t now = time(NULL);
	const unsigned timeout = rfb::Blacklist::initialTimeout;

	if (pthread_mutex_lock(&blmutex))
		abort();

	if (now - timeout > blacklist[addr]) {
		blacklist.erase(addr);
		pthread_mutex_unlock(&blmutex);

		if (pthread_mutex_lock(&hitmutex))
			abort();
		hits.erase(addr);
		pthread_mutex_unlock(&hitmutex);
		return 0;
	} else {
		blacklist[addr] = now;
		pthread_mutex_unlock(&blmutex);
		return 1;
	}
}

void bl_addFailure(const char *addr) {
	if (!rfb::Blacklist::threshold)
		return;

	if (pthread_mutex_lock(&hitmutex))
		abort();
	const unsigned num = ++hits[addr];
	pthread_mutex_unlock(&hitmutex);

	if (num >= (unsigned) rfb::Blacklist::threshold) {
		if (pthread_mutex_lock(&blmutex))
			abort();
		blacklist[addr] = time(NULL);
		pthread_mutex_unlock(&blmutex);
	}
}

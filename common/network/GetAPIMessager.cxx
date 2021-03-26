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

#define __STDC_FORMAT_MACROS

#include <inttypes.h>
#include <network/GetAPI.h>
#include <rfb/ConnParams.h>
#include <rfb/LogWriter.h>
#include <rfb/JpegCompressor.h>
#include <rfb/xxhash.h>
#include <stdio.h>
#include <stdlib.h>

using namespace network;
using namespace rfb;

static LogWriter vlog("GetAPIMessager");

PixelBuffer *progressiveBilinearScale(const PixelBuffer *pb,
					const uint16_t tgtw, const uint16_t tgth,
					const float tgtdiff);

struct TightJPEGConfiguration {
    int quality;
    int subsampling;
};

static const struct TightJPEGConfiguration conf[10] = {
  {  15, subsample4X }, // 0
  {  29, subsample4X }, // 1
  {  41, subsample4X }, // 2
  {  42, subsample2X }, // 3
  {  62, subsample2X }, // 4
  {  77, subsample2X }, // 5
  {  79, subsampleNone }, // 6
  {  86, subsampleNone }, // 7
  {  92, subsampleNone }, // 8
  { 100, subsampleNone }  // 9
};

GetAPIMessager::GetAPIMessager(const char *passwdfile_): passwdfile(passwdfile_),
					screenW(0), screenH(0), screenHash(0),
					cachedW(0), cachedH(0), cachedQ(0) {

	pthread_mutex_init(&screenMutex, NULL);
	pthread_mutex_init(&userMutex, NULL);
}

// from main thread
void GetAPIMessager::mainUpdateScreen(rfb::PixelBuffer *pb) {
	if (pthread_mutex_trylock(&screenMutex))
		return;

	int stride;
	const rdr::U8 * const buf = pb->getBuffer(pb->getRect(), &stride);

	if (pb->width() != screenW || pb->height() != screenH) {
		screenHash = 0;
		screenW = pb->width();
		screenH = pb->height();
		screenPb.setPF(pb->getPF());
		screenPb.setSize(screenW, screenH);

		cachedW = cachedH = cachedQ = 0;
		cachedJpeg.clear();
	}

	const uint64_t newHash = XXH64(buf, pb->area() * 4, 0);
	if (newHash != screenHash) {
		cachedW = cachedH = cachedQ = 0;
		cachedJpeg.clear();

		screenHash = newHash;
		rdr::U8 *rw = screenPb.getBufferRW(screenPb.getRect(), &stride);
		memcpy(rw, buf, screenW * screenH * 4);
		screenPb.commitBufferRW(screenPb.getRect());
	}

	pthread_mutex_unlock(&screenMutex);
}

// from network threads
uint8_t *GetAPIMessager::netGetScreenshot(uint16_t w, uint16_t h,
	const uint8_t q, const bool dedup,
	uint32_t &len, uint8_t *staging) {

	uint8_t *ret = NULL;
	len = 0;

	if (w > screenW)
		w = screenW;
	if (h > screenH)
		h = screenH;

	if (!screenW || !screenH)
		vlog.error("Screenshot requested but no screenshot exists (screen hasn't been viewed)");

	if (!w || !h || q > 9 || !staging)
		return NULL;

	if (pthread_mutex_lock(&screenMutex))
		return NULL;

	if (w == cachedW && h == cachedH && q == cachedQ) {
		if (dedup) {
			// Return the hash of the unchanged image
			sprintf((char *) staging, "%" PRIx64, screenHash);
			ret = staging;
			len = 16;
		} else {
			// Return the cached image
			len = cachedJpeg.size();
			ret = staging;
			memcpy(ret, &cachedJpeg[0], len);

			vlog.info("Returning cached screenshot");
		}
	} else {
		// Encode the new JPEG, cache it
		JpegCompressor jc;
		int quality, subsampling;

		quality = conf[q].quality;
		subsampling = conf[q].subsampling;

		jc.clear();
		int stride;

		if (w != screenW || h != screenH) {
			float xdiff = w / (float) screenW;
			float ydiff = h / (float) screenH;
			const float diff = xdiff < ydiff ? xdiff : ydiff;

			const uint16_t neww = screenW * diff;
			const uint16_t newh = screenH * diff;

			const PixelBuffer *scaled = progressiveBilinearScale(&screenPb, neww, newh, diff);
			const rdr::U8 * const buf = scaled->getBuffer(scaled->getRect(), &stride);

			jc.compress(buf, stride, scaled->getRect(),
					scaled->getPF(), quality, subsampling);

			cachedJpeg.resize(jc.length());
			memcpy(&cachedJpeg[0], jc.data(), jc.length());

			delete scaled;

			vlog.info("Returning scaled screenshot");
		} else {
			const rdr::U8 * const buf = screenPb.getBuffer(screenPb.getRect(), &stride);

			jc.compress(buf, stride, screenPb.getRect(),
					screenPb.getPF(), quality, subsampling);

			cachedJpeg.resize(jc.length());
			memcpy(&cachedJpeg[0], jc.data(), jc.length());

			vlog.info("Returning normal screenshot");
		}

		cachedQ = q;
		cachedW = w;
		cachedH = h;

		len = cachedJpeg.size();
		ret = staging;
		memcpy(ret, &cachedJpeg[0], len);
	}

	pthread_mutex_unlock(&screenMutex);

	return ret;
}

#define USERNAME_LEN sizeof(((struct kasmpasswd_entry_t *)0)->user)
#define PASSWORD_LEN sizeof(((struct kasmpasswd_entry_t *)0)->password)

uint8_t GetAPIMessager::netAddUser(const char name[], const char pw[], const bool write) {
	if (strlen(name) >= USERNAME_LEN) {
		vlog.error("Username too long");
		return 0;
	}

	if (strlen(pw) >= PASSWORD_LEN) {
		vlog.error("Password too long");
		return 0;
	}

	if (!passwdfile)
		return 0;

	action_data act;

	memcpy(act.data.user, name, USERNAME_LEN);
	act.data.user[USERNAME_LEN - 1] = '\0';
	memcpy(act.data.password, pw, PASSWORD_LEN);
	act.data.password[PASSWORD_LEN - 1] = '\0';
	act.data.owner = 0;
	act.data.write = write;

	if (pthread_mutex_lock(&userMutex))
		return 0;

        // This needs to be handled locally for proper interactivity
        // (consider adding users when nobody is connected).
        // The mutex and atomic rename keep things in sync.

        struct kasmpasswd_t *set = readkasmpasswd(passwdfile);
        unsigned s;
        for (s = 0; s < set->num; s++) {
          if (!strcmp(set->entries[s].user, act.data.user)) {
            vlog.error("Can't create user %s, already exists", act.data.user);
            goto out;
          }
        }

        s = set->num++;
        set->entries = (struct kasmpasswd_entry_t *) realloc(set->entries,
                                                                 set->num * sizeof(struct kasmpasswd_entry_t));
        set->entries[s] = act.data;

        writekasmpasswd(passwdfile, set);
        vlog.info("User %s created", act.data.user);
out:
	pthread_mutex_unlock(&userMutex);

	return 1;
}

uint8_t GetAPIMessager::netRemoveUser(const char name[]) {
	if (strlen(name) >= USERNAME_LEN) {
		vlog.error("Username too long");
		return 0;
	}

	action_data act;
	act.action = USER_REMOVE;

	memcpy(act.data.user, name, USERNAME_LEN);
	act.data.user[USERNAME_LEN - 1] = '\0';

	if (pthread_mutex_lock(&userMutex))
		return 0;

	actionQueue.push_back(act);

	pthread_mutex_unlock(&userMutex);

	return 1;
}

uint8_t GetAPIMessager::netGiveControlTo(const char name[]) {
	if (strlen(name) >= USERNAME_LEN) {
		vlog.error("Username too long");
		return 0;
	}

	action_data act;
	act.action = USER_GIVE_CONTROL;

	memcpy(act.data.user, name, USERNAME_LEN);
	act.data.user[USERNAME_LEN - 1] = '\0';

	if (pthread_mutex_lock(&userMutex))
		return 0;

	actionQueue.push_back(act);

	pthread_mutex_unlock(&userMutex);

	return 1;
}

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
#include <network/GetAPIEnums.h>
#include <network/jsonescape.h>
#include <os/CpuStats.h>
#include <rfb/ConnParams.h>
#include <rfb/EncodeManager.h>
#include <rfb/LogWriter.h>
#include <rfb/JpegCompressor.h>
#include <rfb/xxhash.h>
#include <stdio.h>
#include <string>
#include <utility>
#include <fmt/format.h>

using namespace network;
using namespace rfb;

static LogWriter vlog("GetAPIMessager");

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
					cachedW(0), cachedH(0), cachedQ(0),
					ownerConnected(0), activeUsers(0),
					sessionsInfo( "{\"users\":[]}"){

	pthread_mutex_init(&screenMutex, nullptr);
	pthread_mutex_init(&userMutex, nullptr);
	pthread_mutex_init(&statMutex, nullptr);
	pthread_mutex_init(&frameStatMutex, nullptr);
	pthread_mutex_init(&userInfoMutex, nullptr);

	serverFrameStats.inprogress = 0;
}

// from main thread
void GetAPIMessager::mainUpdateScreen(rfb::PixelBuffer *pb) {
    if (!pb)
        return;

    if (pthread_mutex_trylock(&screenMutex))
        return;

    int stride;
    TRACE_STOPWATCH(shotstart);

    const rdr::U8 *const buf = pb->getBuffer(pb->getRect(), &stride);

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

    if (!pthread_mutex_lock(&frameStatMutex)) {
        serverFrameStats.shot = msSince(&shotstart);
        pthread_mutex_unlock(&frameStatMutex);
    }

    TRACE_STOPWATCH_PRINT_MS(vlog, shotstart);
    pthread_mutex_unlock(&screenMutex);
}

void GetAPIMessager::mainUpdateBottleneckStats(const char userid[], const char stats[]) {
	if (pthread_mutex_trylock(&statMutex))
		return;

	bottleneckStats[userid] = stats;

	pthread_mutex_unlock(&statMutex);
}

void GetAPIMessager::mainClearBottleneckStats(const char userid[]) {
	if (pthread_mutex_lock(&statMutex))
		return;

	bottleneckStats.erase(userid);

	pthread_mutex_unlock(&statMutex);
}

void GetAPIMessager::mainUpdateServerFrameStats(uint8_t changedPerc,
	uint32_t all, uint32_t jpeg, uint32_t webp, uint32_t analysis,
	uint32_t jpegarea, uint32_t webparea,
	uint16_t njpeg, uint16_t nwebp,
	uint16_t enc, uint16_t scale,
	uint16_t w, uint16_t h) {

	if (pthread_mutex_lock(&frameStatMutex))
		return;

	serverFrameStats.changedPerc = changedPerc;
	serverFrameStats.all = all;
	serverFrameStats.jpeg = jpeg;
	serverFrameStats.webp = webp;
	serverFrameStats.analysis = analysis;
	serverFrameStats.jpegarea = jpegarea;
	serverFrameStats.webparea = webparea;
	serverFrameStats.njpeg = njpeg;
	serverFrameStats.nwebp = nwebp;
	serverFrameStats.enc = enc;
	serverFrameStats.scale = scale;
	serverFrameStats.w = w;
	serverFrameStats.h = h;

	pthread_mutex_unlock(&frameStatMutex);
}

void GetAPIMessager::mainUpdateClientFrameStats(const char userid[], uint32_t render,
	uint32_t all, uint32_t ping) {

	if (pthread_mutex_lock(&frameStatMutex))
		return;

	clientFrameStats_t s;
	s.render = render;
	s.all = all;
	s.ping = ping;

	clientFrameStats[userid] = s;

	pthread_mutex_unlock(&frameStatMutex);
}

void GetAPIMessager::mainUpdateUserInfo(const uint8_t ownerConn, const uint8_t numUsers) {
	if (pthread_mutex_lock(&userInfoMutex))
		return;

	ownerConnected = ownerConn;
	activeUsers = numUsers;

	pthread_mutex_unlock(&userInfoMutex);
}

void GetAPIMessager::mainUpdateSessionsInfo(std::string newSessionsInfo)
{
	 std::unique_lock<std::mutex> lock (sessionInfoMutex,std::defer_lock);
	if (!lock.try_lock())
		return;
	sessionsInfo = std::move(newSessionsInfo);
	lock.unlock();
}

// from network threads
uint8_t *GetAPIMessager::netGetScreenshot(uint16_t w, uint16_t h,
	const uint8_t q, const bool dedup,
	uint32_t &len, uint8_t *staging) {
	uint8_t *ret = nullptr;
	len = 0;

	if (q > 9 || !staging)
		return nullptr;

	if (pthread_mutex_lock(&screenMutex))
	    return nullptr;

    if (w > screenW)
        w = screenW;
    if (h > screenH)
        h = screenH;

    if (!w || !h) {
        vlog.error("Screenshot requested but no screenshot exists (screen hasn't been viewed)");
        pthread_mutex_unlock(&screenMutex);

        return nullptr;
    }

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

		const int quality = conf[q].quality;
		const int subsampling = conf[q].subsampling;

		jc.clear();
		int stride;

		if (w != screenW || h != screenH) {
			const float xdiff = w / (float) screenW;
			const float ydiff = h / (float) screenH;
			const float diff = xdiff < ydiff ? xdiff : ydiff;

			const uint16_t neww = screenW * diff;
			const uint16_t newh = screenH * diff;

			const PixelBuffer *scaled = progressiveBilinearScale(&screenPb, neww, newh, diff);
            const rdr::U8 *const buf = scaled->getBuffer(scaled->getRect(), &stride);

			jc.compress(buf, stride, scaled->getRect(),
					scaled->getPF(), quality, subsampling);

			cachedJpeg.resize(jc.length());
			memcpy(&cachedJpeg[0], jc.data(), jc.length());

			delete scaled;

			vlog.info("Returning scaled screenshot");
		} else {
            const rdr::U8 *const buf = screenPb.getBuffer(screenPb.getRect(), &stride);

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

uint8_t GetAPIMessager::netAddUser(const char name[], const char pw[],
					const bool read, const bool write,
					const bool owner) {
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

	uint8_t ret = 1;

	action_data act;

	memcpy(act.data.user, name, USERNAME_LEN);
	act.data.user[USERNAME_LEN - 1] = '\0';
	memcpy(act.data.password, pw, PASSWORD_LEN);
	act.data.password[PASSWORD_LEN - 1] = '\0';
	act.data.owner = owner;
	act.data.write = write;
	act.data.read = read;

	if (pthread_mutex_lock(&userMutex))
		return 0;

        // This needs to be handled locally for proper interactivity
        // (consider adding users when nobody is connected).
        // The mutex and atomic rename keep things in sync.

        struct kasmpasswd_t *set = readkasmpasswd(passwdfile);
        unsigned s;
        for (s = 0; s < set->num; s++) {
          if (!strcmp(set->entries[s].user, name)) {
            vlog.error("Can't create user %s, already exists", name);
            ret = 0;
            goto out;
          }
        }

        s = set->num++;
        set->entries = (struct kasmpasswd_entry_t *) realloc(set->entries,
                                                                 set->num * sizeof(struct kasmpasswd_entry_t));
        set->entries[s] = act.data;

        writekasmpasswd(passwdfile, set);
        vlog.info("User %s created", name);
out:
	pthread_mutex_unlock(&userMutex);

	free(set->entries);
	free(set);

	return ret;
}

uint8_t GetAPIMessager::netRemoveUser(const char name[]) {
	if (strlen(name) >= USERNAME_LEN) {
		vlog.error("Username too long");
		return 0;
	}

	if (pthread_mutex_lock(&userMutex))
		return 0;

	struct kasmpasswd_t *set = readkasmpasswd(passwdfile);
	bool found = false;
	for (unsigned s = 0; s < set->num; s++) {
		if (!strcmp(set->entries[s].user, name)) {
			set->entries[s].user[0] = '\0';
			found = true;
			break;
		}
	}

	if (found) {
		writekasmpasswd(passwdfile, set);
		vlog.info("User %s removed", name);
	} else {
		vlog.error("Tried to remove nonexistent user %s", name);

		pthread_mutex_unlock(&userMutex);

		free(set->entries);
		free(set);

		return 0;
	}

	pthread_mutex_unlock(&userMutex);

	free(set->entries);
	free(set);

	return 1;
}

uint8_t GetAPIMessager::netUpdateUser(const char name[], const uint64_t mask,
	                              const char password[],
	                              const bool read, const bool write, const bool owner) {
	if (strlen(name) >= USERNAME_LEN) {
		vlog.error("Username too long");
		return 0;
	}

	if (strlen(password) >= PASSWORD_LEN) {
		vlog.error("Password too long");
		return 0;
	}

	if (!mask) {
		vlog.error("Update_user without any updates?");
		return 0;
	}

	if (pthread_mutex_lock(&userMutex))
		return 0;

	struct kasmpasswd_t *set = readkasmpasswd(passwdfile);
	bool found = false;
	for (unsigned s = 0; s < set->num; s++) {
		if (!strcmp(set->entries[s].user, name)) {
			if (mask & USER_UPDATE_READ_MASK)
				set->entries[s].read = read;
			if (mask & USER_UPDATE_WRITE_MASK)
				set->entries[s].write = write;
			if (mask & USER_UPDATE_OWNER_MASK)
				set->entries[s].owner = owner;

			if (mask & USER_UPDATE_PASSWORD_MASK)
				strcpy(set->entries[s].password, password);
			found = true;
			break;
		}
	}

	if (found) {
		writekasmpasswd(passwdfile, set);
		vlog.info("User %s permissions updated", name);
	} else {
		vlog.error("Tried to update nonexistent user %s", name);

		pthread_mutex_unlock(&userMutex);

		free(set->entries);
		free(set);

		return 0;
	}

	pthread_mutex_unlock(&userMutex);

	free(set->entries);
	free(set);

	return 1;
}

uint8_t GetAPIMessager::netAddOrUpdateUser(const struct kasmpasswd_entry_t *entry) {

	if (pthread_mutex_lock(&userMutex))
		return 0;

        struct kasmpasswd_t *set = readkasmpasswd(passwdfile);
        unsigned s;
        bool updated = false;
        for (s = 0; s < set->num; s++) {
		if (!strcmp(set->entries[s].user, entry->user)) {
			set->entries[s] = *entry;
			updated = true;
			vlog.info("User %s updated", entry->user);
			break;
		}
        }

	if (!updated) {
	        s = set->num++;
		set->entries = (struct kasmpasswd_entry_t *) realloc(set->entries,
									set->num * sizeof(struct kasmpasswd_entry_t));
		set->entries[s] = *entry;
	        vlog.info("User %s created", entry->user);
        }

	writekasmpasswd(passwdfile, set);

	pthread_mutex_unlock(&userMutex);

	free(set->entries);
	free(set);

	return 1;

}

void GetAPIMessager::netGetUsers(const char **outptr) {
/*
[
    { "user": "username", "write": true, "owner": true },
    { "user": "username", "write": true, "owner": true }
]
*/
	char escapeduser[USERNAME_LEN * 2];

	if (pthread_mutex_lock(&userMutex)) {
		*outptr = (char *) calloc(1, 1);
		return;
	}

	struct kasmpasswd_t *set = readkasmpasswd(passwdfile);

	auto *buf = (char *) calloc(set->num, 80);
	FILE *f = fmemopen(buf, set->num * 80, "w");

	fprintf(f, "[\n");

	for (unsigned s = 0; s < set->num; s++) {
		JSON_escape(set->entries[s].user, escapeduser);

		fprintf(f, "    { \"user\": \"%s\", \"read\": %s, \"write\": %s, \"owner\": %s }",
			escapeduser,
			set->entries[s].read ? "true" : "false",
			set->entries[s].write ? "true" : "false",
			set->entries[s].owner ? "true" : "false");

		if (s == set->num - 1)
			fprintf(f, "\n");
		else
			fprintf(f, ",\n");
	}

	free(set->entries);
	free(set);

	fprintf(f, "]\n");

	fclose(f);

	pthread_mutex_unlock(&userMutex);
	*outptr = buf;
}


const std::string_view GetAPIMessager::netGetSessions()
{
	return sessionsInfo;
}

void GetAPIMessager::netGetBottleneckStats(char *buf, uint32_t len) {
/*
{
    "username.1": {
        "192.168.100.2:14908": [ 100, 100, 100, 100 ],
        "192.168.100.3:14918": [ 100, 100, 100, 100 ]
    },
    "username.2": {
        "192.168.100.5:14904": [ 100, 100, 100, 100 ]
    }
}
*/
	std::map<std::string, std::string>::const_iterator it;
    const char *prev = nullptr;
	FILE *f;

	if (pthread_mutex_lock(&statMutex)) {
		buf[0] = 0;
		return;
	}

	// Conservative estimate
	if (len < bottleneckStats.size() * 60) {
		buf[0] = 0;
		goto out;
	}

	f = fmemopen(buf, len, "w");

	fprintf(f, "{\n");

	for (auto it = bottleneckStats.begin(); it != bottleneckStats.end(); it++) {
		// user@127.0.0.1_1627311208.791752::websocket
		const char *id = it->first.c_str();
		const char *data = it->second.c_str();

		const char *at = strrchr(id, '@');
		if (!at)
			continue;

		const unsigned userlen = at - id;
		if (prev && !strncmp(prev, id, userlen)) {
			// Same user
			fprintf(f, ",\n\t\t\"%s\": %s", at + 1, data);
		} else {
			// New one
			if (prev) {
				fprintf(f, "\n\t},\n");
			}
			fprintf(f, "\t\"%.*s\": {\n", userlen, id);
			fprintf(f, "\t\t\"%s\": %s", at + 1, data);
		}

		prev = id;
	}

	if (!bottleneckStats.size())
		fprintf(f, "}\n");
	else
		fprintf(f, "\n\t}\n}\n");

	fclose(f);

out:
	pthread_mutex_unlock(&statMutex);
}

void GetAPIMessager::netGetFrameStats(char *buf, uint32_t len) {
/*
{
	"frame" : {
		"resx": 1024,
		"resy": 1280,
		"changed": 75,
		"server_time": 23
	},
	"server_side" : [
		{ "process_name": "Analysis", "time": 20 },
		{ "process_name": "TightWEBPEncoder", "time": 20, "count": 64, "area": 12 },
		{ "process_name": "TightJPEGEncoder", "time": 20, "count": 64, "area": 12 }
	],
	"client_side" : [
		{
			"client": "123.1.2.1:1211",
			"client_time": 20,
			"ping": 20,
			"processes" : [
				{ "process_name": "scanRenderQ", "time": 20 }
			]
		}
	}
}
*/
    unsigned i = 0;
	FILE *f;

	if (pthread_mutex_lock(&frameStatMutex)) {
		buf[0] = 0;
		return;
	}

	const unsigned num = clientFrameStats.size();

	// Conservative estimate
	if (len < 1024) {
		buf[0] = 0;
		goto out;
	}

	f = fmemopen(buf, len, "w");

	fprintf(f, "{\n");

	fprintf(f, "\t\"frame\" : {\n"
	           "\t\t\"resx\": %u,\n"
	           "\t\t\"resy\": %u,\n"
	           "\t\t\"changed\": %u,\n"
	           "\t\t\"server_time\": %u\n"
	           "\t},\n",
	           serverFrameStats.w,
	           serverFrameStats.h,
	           serverFrameStats.changedPerc,
	           serverFrameStats.all);

	fprintf(f, "\t\"server_side\" : [\n"
	           "\t\t{ \"process_name\": \"Analysis\", \"time\": %u },\n"
	           "\t\t{ \"process_name\": \"Screenshot\", \"time\": %u },\n"
	           "\t\t{ \"process_name\": \"Encoding_total\", \"time\": %u, \"videoscaling\": %u },\n"
	           "\t\t{ \"process_name\": \"TightJPEGEncoder\", \"time\": %u, \"count\": %u, \"area\": %u },\n"
	           "\t\t{ \"process_name\": \"TightWEBPEncoder\", \"time\": %u, \"count\": %u, \"area\": %u }\n"
	           "\t],\n",
	           serverFrameStats.analysis,
	           serverFrameStats.shot,
	           serverFrameStats.enc,
	           serverFrameStats.scale,
	           serverFrameStats.jpeg,
	           serverFrameStats.njpeg,
	           serverFrameStats.jpegarea,
	           serverFrameStats.webp,
	           serverFrameStats.nwebp,
	           serverFrameStats.webparea);

	fprintf(f, "\t\"client_side\" : [\n");

	for (auto it = clientFrameStats.begin(); it != clientFrameStats.end(); it++, i++) {
		const char *id = it->first.c_str();
		const clientFrameStats_t &s = it->second;

		fprintf(f, "\t\t\{\n"
		           "\t\t\t\"client\": \"%s\",\n"
		           "\t\t\t\"client_time\": %u,\n"
		           "\t\t\t\"ping\": %u,\n"
		           "\t\t\t\"processes\" : [\n"
		           "\t\t\t\t{ \"process_name\": \"scanRenderQ\", \"time\": %u }\n"
		           "\t\t\t]\n"
		           "\t\t}",
		           id,
		           s.all,
		           s.ping,
		           s.render);

		if (i == num - 1)
			fprintf(f, "\n");
		else
			fprintf(f, ",\n");
	}

	fprintf(f, "\t]\n}\n");

	fclose(f);

	serverFrameStats.inprogress = 0;

out:
	pthread_mutex_unlock(&frameStatMutex);
}

void GetAPIMessager::netResetFrameStatsCall() {
	if (pthread_mutex_lock(&frameStatMutex))
		return;

	serverFrameStats.inprogress = 0;

	pthread_mutex_unlock(&frameStatMutex);
}

uint8_t GetAPIMessager::netRequestFrameStats(USER_ACTION what, const char *client) {
	// Return 1 for success
	action_data act;
	act.action = what;
	if (client) {
		strncpy(act.data.password, client, PASSWORD_LEN);
		act.data.password[PASSWORD_LEN - 1] = '\0';
	}

	// In progress already?
	bool fail = false;
	if (pthread_mutex_lock(&frameStatMutex))
		return 0;

	if (serverFrameStats.inprogress) {
		fail = true;
		vlog.error("Frame stats request already in progress, refusing another");
	} else {
		clientFrameStats.clear();
		memset(&serverFrameStats, 0, sizeof(serverFrameStats_t));
		serverFrameStats.inprogress = 1;
	}

	pthread_mutex_unlock(&frameStatMutex);
	if (fail)
		return 0;

	// Send it in
	if (pthread_mutex_lock(&userMutex))
		return 0;

	actionQueue.push_back(act);

	pthread_mutex_unlock(&userMutex);

	return 1;
}

uint8_t GetAPIMessager::netOwnerConnected() {
	if (pthread_mutex_lock(&userInfoMutex))
		return 0;

	uint8_t ret = ownerConnected;

	pthread_mutex_unlock(&userInfoMutex);

	return ret;
}

uint8_t GetAPIMessager::netNumActiveUsers() {
	if (pthread_mutex_lock(&userInfoMutex))
		return 0;

	uint8_t ret = activeUsers;

	pthread_mutex_unlock(&userInfoMutex);

	return ret;
}

uint8_t GetAPIMessager::netGetClientFrameStatsNum() {
	if (pthread_mutex_lock(&frameStatMutex))
		return 0;

	uint8_t ret = clientFrameStats.size();

	pthread_mutex_unlock(&frameStatMutex);

	return ret;
}

uint8_t GetAPIMessager::netServerFrameStatsReady() {
	if (pthread_mutex_lock(&frameStatMutex))
		return 0;

	uint8_t ret = serverFrameStats.w != 0;

	pthread_mutex_unlock(&frameStatMutex);

	return ret;
}

void GetAPIMessager::netUdpUpgrade(void *client, uint32_t ip) {
	// Return 1 for success
	action_data act;
	act.action = UDP_UPGRADE;
	act.udp.client = client;
	act.udp.ip = ip;

	// Send it in
	if (pthread_mutex_lock(&userMutex))
		return;

	actionQueue.push_back(act);

	pthread_mutex_unlock(&userMutex);
}

void GetAPIMessager::netClearClipboard() {
	action_data act;
	act.action = CLEAR_CLIPBOARD;

	// Send it in
	if (pthread_mutex_lock(&userMutex))
		return;

	actionQueue.push_back(act);

	pthread_mutex_unlock(&userMutex);
}

void GetAPIMessager::netUpdateSystemStats() {
	const auto host_cpu_usage = SystemStats::get_cpu_usage();
	const auto mem_stats = system_stats.get_mem_stats();
	const auto io_stats = system_stats.get_io_stats();
	const auto cgroup_limits = CgroupStats::get_limits();
	const auto cgroup_cpu_stats = CpuStats::get_cpu_stats(cgroup_limits);

	fmt::memory_buffer buf;

	fmt::format_to(std::back_inserter(buf),
	               "{{\n"
	               "\t\"cpu\":{{\n"
	               "\t\t\"usage_percent\": {},\n"
	               "\t\t\"has_cgroup_usage\": {},\n"
	               "\t\t\"cgroup_usage_percent\": {},\n"
	               "\t\t\"cgroup_available_percent\": {},\n"
	               "\t\t\"cgroup_effective_cores\": {},\n"
	               "\t\t\"cgroup_usage_usec\": {},\n"
	               "\t\t\"cgroup_user_usec\": {},\n"
	               "\t\t\"cgroup_system_usec\": {}}},\n"
	               "\t\"memory\":{{\n"
	               "\t\t\"total\": {},\n"
	               "\t\t\"free\": {},\n"
	               "\t\t\"cached\": {},\n"
	               "\t\t\"used\": {}}},\n"
	               "\t\"cgroup\":{{\n"
	               "\t\t\"has_mem_limit\": {},\n"
	               "\t\t\"mem_limit\": {},\n"
	               "\t\t\"has_cpu_limit\": {},\n"
	               "\t\t\"cpu_limit_cores\": {},\n"
	               "\t\t\"has_cpu_weight\": {},\n"
	               "\t\t\"cpu_weight\": {},\n"
	               "\t\t\"has_cpu_affinity\": {},\n"
	               "\t\t\"cpu_affinity\": \"{}\"}},\n"
	               "\t\"cpu_throttling\":{{\n"
	               "\t\t\"available\": {},\n"
	               "\t\t\"has_throttling\": {},\n"
	               "\t\t\"nr_periods\": {},\n"
	               "\t\t\"nr_throttled\": {},\n"
	               "\t\t\"throttled_usec\": {},\n"
	               "\t\t\"throttled_percent\": {}}},\n"
	               "\t\"io_stats\":{{\n",
	               host_cpu_usage, cgroup_cpu_stats.available,
	               cgroup_cpu_stats.usage_percent, cgroup_cpu_stats.availability_percent,
	               cgroup_cpu_stats.effective_cores, cgroup_cpu_stats.usage_usec,
	               cgroup_cpu_stats.user_usec, cgroup_cpu_stats.system_usec,
	               mem_stats.total, mem_stats.free, mem_stats.cached, mem_stats.used,
	               cgroup_limits.has_mem_limit, cgroup_limits.mem_limit,
	               cgroup_limits.has_cpu_limit, cgroup_limits.cpu_limit_cores,
	               cgroup_limits.has_cpu_weight, cgroup_limits.cpu_weight,
	               cgroup_limits.has_cpu_affinity, cgroup_limits.cpu_affinity,
	               cgroup_cpu_stats.available, cgroup_cpu_stats.has_throttling, cgroup_cpu_stats.nr_periods,
	               cgroup_cpu_stats.nr_throttled, cgroup_cpu_stats.throttled_usec,
	               cgroup_cpu_stats.throttled_percent);

	for (size_t i = 0; i < io_stats.size(); ++i) {
		const auto& dev_io_stats = io_stats[i];
		fmt::format_to(std::back_inserter(buf),
			"\t\t\"{}\": {{\n"
			"\t\t\t\"bytes_read\": {},\n"
			"\t\t\t\"bytes_read_per_sec\": {},\n"
			"\t\t\t\"bytes_written\": {},\n"
			"\t\t\t\"bytes_written_per_sec\": {},\n"
			"\t\t\t\"iowait\": {} }}{}\n",
			dev_io_stats.disk_name, dev_io_stats.bytes_read, dev_io_stats.bytes_read_per_sec, dev_io_stats.bytes_written,  dev_io_stats.bytes_written_per_sec, dev_io_stats.iowait, i + 1 < io_stats.size() ? "," : "");
	}

	fmt::format_to(std::back_inserter(buf), "}}}}\n");

	std::lock_guard lock(system_stats_mutex);

	systems_stats_json = fmt::to_string(buf);
}

void GetAPIMessager::netGetSystemStats(const char **ptr, uint32_t *len) {
	thread_local std::string local_copy;

	std::lock_guard lock(system_stats_mutex);
	local_copy = systems_stats_json;

	*ptr = local_copy.c_str();
	*len = local_copy.size();
}

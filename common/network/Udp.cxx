/* Copyright (C) Kasm
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
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>

#include <network/GetAPI.h>
#include <network/Udp.h>
#include <network/webudp/WuHost.h>
#include <network/webudp/Wu.h>
#include <network/websocket.h>
#include <rfb/LogWriter.h>
#include <rfb/ServerCore.h>
#include <rfb/xxhash.h>

using namespace network;

static rfb::LogWriter vlog("WebUdp");
static WuHost *host = NULL;

rfb::IntParameter udpSize("udpSize", "UDP packet data size", 1296, 500, 1400);

extern settings_t settings;

static void udperr(const char *msg, void *) {
	vlog.error("%s", msg);
}

static void udpdebug(const char *msg, void *) {
	vlog.debug("%s", msg);
}

void *udpserver(void *nport) {

	WuHost *myhost = NULL;
	int ret = WuHostCreate(rfb::Server::publicIP, *(uint16_t *) nport, 16, &myhost);
	if (ret != WU_OK) {
		vlog.error("Failed to create WebUDP host");
		return NULL;
	}
	vlog.debug("UDP listening on port %u", *(uint16_t *) nport);

	__sync_bool_compare_and_swap(&host, host, myhost);

	GetAPIMessager *msgr = (GetAPIMessager *) settings.messager;

	WuHostSetErrorCallback(host, udperr);
	WuHostSetDebugCallback(host, udpdebug);

	while (1) {
		WuAddress addr;
		WuEvent e;
		if (!WuHostServe(host, &e, 2000))
			continue;

		switch (e.type) {
			case WuEvent_ClientJoin:
				vlog.info("client join");
				addr = WuClientGetAddress(e.client);
				msgr->netUdpUpgrade(e.client, htonl(addr.host));
			break;
			case WuEvent_ClientLeave:
				vlog.info("client leave");
				WuHostRemoveClient(host, e.client);
			break;
			default:
				vlog.error("client sent data, this is unexpected");
			break;
		}
	}

	return NULL;
}

// Send one packet, split into N UDP-sized pieces
static uint8_t udpsend(WuClient *client, const uint8_t *data, unsigned len, uint32_t *id,
			const uint32_t *frame) {
	const uint32_t DATA_MAX = udpSize;

	uint8_t buf[1400 + sizeof(uint32_t) * 5];
	const uint32_t pieces = (len / DATA_MAX) + ((len % DATA_MAX) ? 1 : 0);

	uint32_t i;

	for (i = 0; i < pieces; i++) {
		const unsigned curlen = len > DATA_MAX ? DATA_MAX : len;
		const uint32_t hash = XXH64(data, curlen, 0);

		memcpy(buf, id, sizeof(uint32_t));
		memcpy(&buf[4], &i, sizeof(uint32_t));
		memcpy(&buf[8], &pieces, sizeof(uint32_t));
		memcpy(&buf[12], &hash, sizeof(uint32_t));
		memcpy(&buf[16], frame, sizeof(uint32_t));

		memcpy(&buf[20], data, curlen);
		data += curlen;
		len -= curlen;

		if (WuHostSendBinary(host, client, buf, curlen + sizeof(uint32_t) * 5) < 0)
			return 1;
	}

	(*id)++;

	return 0;
}

UdpStream::UdpStream(): OutStream(), client(NULL), total_len(0), id(0), failed(false),
	                frame(0) {
	ptr = data;
	end = data + UDPSTREAM_BUFSIZE;

	srand(time(NULL));
}

void UdpStream::flush() {
	const unsigned len = ptr - data;
	total_len += len;

	if (client) {
		if (udpsend(client, data, len, &id, &frame)) {
			vlog.error("Error sending udp, client gone?");
			failed = true;
		}
	} else {
		vlog.error("Tried to send udp without a client");
	}

	ptr = data;
}

void UdpStream::overrun(size_t needed) {
	vlog.error("Udp buffer overrun");
	abort();
}

bool UdpStream::isFailed() const {
	return failed;
}

void UdpStream::clearFailed() {
	failed = false;
}

void wuGotHttp(const char msg[], const uint32_t msglen, char resp[]) {
	WuGotHttp(host, msg, msglen, resp);
}

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
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <network/iceip.h>
#include <rfb/LogWriter.h>
#include <rfb/ServerCore.h>

static rfb::LogWriter vlog("ICE");

// Default port 3478
static const char * const servers[] = {
	"stun.l.google.com:19302",
	"stun1.l.google.com:19302",
	"stun2.l.google.com:19302",
	"stun3.l.google.com:19302",
	"stun4.l.google.com:19302",
	"stun.voipbuster.com",
	"stun.voipstunt.com",
};

static bool tryserver(const char * const srv, const int sock) {

	unsigned port = 3478;
	char addr[PATH_MAX];
	char buf[PATH_MAX];

	const char *colon = strchr(srv, ':');
	if (colon) {
		memcpy(addr, srv, colon - srv);
		addr[colon - srv] = '\0';

		colon++;
		port = atoi(colon);
	} else {
		strcpy(addr, srv);
	}

	vlog.debug("Trying '%s', port %u", addr, port);

	struct hostent *ent = gethostbyname2(addr, AF_INET);
	if (!ent)
		return false;

	struct sockaddr_in dst;
	dst.sin_family = AF_INET;
	dst.sin_port = htons(port);
	memcpy(&dst.sin_addr, ent->h_addr, 4);
	//vlog.info("Got %s, addr %s", ent->h_name, inet_ntoa(in));

	// Build up a binding request packet
	buf[0] = 0;
	buf[1] = 1; // type
	buf[2] = buf[3] = 0; // length

	uint32_t tid[4]; // transaction id, 128 bits
	tid[0] = rand();
	tid[1] = rand();
	tid[2] = rand();
	tid[3] = rand();

	memcpy(&buf[4], &tid[0], 4);
	memcpy(&buf[8], &tid[1], 4);
	memcpy(&buf[12], &tid[2], 4);
	memcpy(&buf[16], &tid[3], 4);

	if (sendto(sock, buf, 20, 0, (const struct sockaddr *) &dst,
		sizeof(struct sockaddr_in)) != 20)
		return false;

	// Wait up to 10s for a reply, standard says that's the wait
	struct pollfd pfd;
	pfd.fd = sock;
	pfd.events = POLLIN;
	if (poll(&pfd, 1, 10 * 1000) <= 0)
		return false;

	struct sockaddr_in from;
	socklen_t socklen = sizeof(struct sockaddr_in);
	int len = recvfrom(sock, buf, PATH_MAX, 0, (struct sockaddr *) &from,
				&socklen);
	if (len < 20)
		return false;
	if (memcmp(&from.sin_addr, &dst.sin_addr, sizeof(struct in_addr)))
		return false;

	int i;
/*	vlog.info("Got %u bytes", len);
	for (i = 0; i < len; i++)
		vlog.info("0x%02x,", buf[i]);*/

	if (buf[0] != 1 || buf[1] != 1)
		return false; // type not binding response

	// Parse attrs
	for (i = 20; i < len;) {
		uint16_t type, attrlen;
		memcpy(&type, &buf[i], 2);
		i += 2;
		memcpy(&attrlen, &buf[i], 2);
		i += 2;

		type = ntohs(type);
		attrlen = ntohs(attrlen);
		if (type != 1) {
			// Not mapped-address
			i += attrlen;
			continue;
		}

		// Yay, we got a response
		i += 4;
		struct in_addr in;
		memcpy(&in.s_addr, &buf[i], 4);

		rfb::Server::publicIP.setParam(inet_ntoa(in));

		vlog.info("My public IP is %s", (const char *) rfb::Server::publicIP);

		return true;
	}

	return false;
}

void getPublicIP() {
	if (rfb::Server::publicIP[0]) {
		vlog.info("Using public IP %s from args",
				(const char *) rfb::Server::publicIP);
		return;
	}

	srand(time(NULL));

	vlog.info("Querying public IP...");

	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		abort();

	unsigned i;
	if (rfb::Server::stunServer[0]) {
		if (strlen(rfb::Server::stunServer) < PATH_MAX)
			tryserver(rfb::Server::stunServer, sock);
	} else {
		for (i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
			if (tryserver(servers[i], sock))
				break;
			vlog.info("STUN server %u didn't work, trying next...", i);
		}
	}

	close(sock);

	if (!rfb::Server::publicIP[0]) {
		vlog.error("Failed to get public IP, please specify it with -publicIP");
		exit(1);
	}
}

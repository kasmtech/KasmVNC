// Regression for the libjuice zero ICE tiebreaker backport (libjuice PR #356).
// A controlling agent must accept a Binding request whose ICE-CONTROLLED
// tiebreaker is zero instead of answering 400 Bad Request.
#include <juice/juice.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define AGENT_UFRAG "kvncagnt"
#define AGENT_PWD "kvncagentpassword000000"
#define PEER_UFRAG "kvncpeer"
#define PEER_PWD "kvncpeerpassword0000000"
#define STUN_MAGIC 0x2112A442u

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	size_t padded = ((len + 8) / 64 + 1) * 64;
	uint8_t *buf = calloc(padded, 1);
	memcpy(buf, data, len);
	buf[len] = 0x80;
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++)
		buf[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
	for (size_t off = 0; off < padded; off += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)buf[off + 4 * i] << 24 | (uint32_t)buf[off + 4 * i + 1] << 16 |
			       (uint32_t)buf[off + 4 * i + 2] << 8 | buf[off + 4 * i + 3];
		for (int i = 16; i < 80; i++)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDC;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6;
			}
			uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rol(b, 30);
			b = a;
			a = t;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	free(buf);
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 4; j++)
			out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

static void hmac_sha1(const char *key, const uint8_t *msg, size_t len, uint8_t out[20]) {
	uint8_t k[64] = {0}, inner[20];
	memcpy(k, key, strlen(key));
	uint8_t *buf = malloc(64 + len);
	for (int i = 0; i < 64; i++)
		buf[i] = k[i] ^ 0x36;
	memcpy(buf + 64, msg, len);
	sha1(buf, 64 + len, inner);
	uint8_t outer[64 + 20];
	for (int i = 0; i < 64; i++)
		outer[i] = k[i] ^ 0x5c;
	memcpy(outer + 64, inner, 20);
	sha1(outer, sizeof(outer), out);
	free(buf);
}

static uint32_t crc32(const uint8_t *data, size_t len) {
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int j = 0; j < 8; j++)
			crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
	}
	return ~crc;
}

static size_t put_attr(uint8_t *p, uint16_t type, const void *value, uint16_t len) {
	p[0] = type >> 8;
	p[1] = type & 0xff;
	p[2] = len >> 8;
	p[3] = len & 0xff;
	memcpy(p + 4, value, len);
	size_t padded = (len + 3) & ~3u;
	memset(p + 4 + len, 0, padded - len);
	return 4 + padded;
}

static void set_length(uint8_t *msg, size_t attrs) {
	msg[2] = (uint8_t)(attrs >> 8);
	msg[3] = (uint8_t)attrs;
}

static size_t binding_request(uint8_t *msg, const uint8_t txid[12]) {
	msg[0] = 0x00;
	msg[1] = 0x01;
	uint32_t magic = htonl(STUN_MAGIC);
	memcpy(msg + 4, &magic, 4);
	memcpy(msg + 8, txid, 12);
	size_t pos = 20;
	const char *username = AGENT_UFRAG ":" PEER_UFRAG;
	pos += put_attr(msg + pos, 0x0006, username, (uint16_t)strlen(username));
	const uint8_t zero_tiebreaker[8] = {0};
	pos += put_attr(msg + pos, 0x8029, zero_tiebreaker, 8);
	const uint8_t priority[4] = {0x6e, 0x00, 0x01, 0xff};
	pos += put_attr(msg + pos, 0x0024, priority, 4);

	set_length(msg, pos - 20 + 24);
	uint8_t mac[20];
	hmac_sha1(AGENT_PWD, msg, pos, mac);
	pos += put_attr(msg + pos, 0x0008, mac, 20);

	set_length(msg, pos - 20 + 8);
	uint32_t fingerprint = htonl(crc32(msg, pos) ^ 0x5354554e);
	pos += put_attr(msg + pos, 0x8028, &fingerprint, 4);
	return pos;
}

static volatile int gathered;
static void on_gathering_done(juice_agent_t *agent, void *user) {
	(void)agent;
	(void)user;
	gathered = 1;
}

static int host_candidate_port(const char *sdp) {
	for (const char *line = strstr(sdp, "a=candidate:"); line; line = strstr(line + 1, "a=candidate:")) {
		char address[64];
		int port;
		char type[16];
		if (sscanf(line, "a=candidate:%*s %*d %*s %*u %63s %d typ %15s", address, &port, type) == 3 &&
		    strcmp(type, "host") == 0 && strchr(address, ':') == NULL)
			return port;
	}
	return -1;
}

static int fail(const char *why) {
	fprintf(stderr, "libjuice zero tiebreaker regression: %s\n", why);
	return 1;
}

int main(void) {
	juice_set_log_level(JUICE_LOG_LEVEL_NONE);

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in peer = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	socklen_t peer_len = sizeof(peer);
	if (sock < 0 || bind(sock, (struct sockaddr *)&peer, sizeof(peer)) < 0 ||
	    getsockname(sock, (struct sockaddr *)&peer, &peer_len) < 0)
		return fail("cannot bind peer socket");
	struct timeval timeout = {.tv_sec = 1};
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	juice_config_t config = {.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL,
	                         .cb_gathering_done = on_gathering_done};
	juice_agent_t *agent = juice_create(&config);
	if (!agent || juice_set_local_ice_attributes(agent, AGENT_UFRAG, AGENT_PWD) < 0)
		return fail("cannot create agent");

	// Describing the local side first makes the agent controlling, like KasmVNC's offerer.
	char sdp[4096];
	if (juice_get_local_description(agent, sdp, sizeof(sdp)) < 0)
		return fail("cannot get local description");
	char remote[512];
	snprintf(remote, sizeof(remote),
	         "a=ice-ufrag:" PEER_UFRAG "\r\na=ice-pwd:" PEER_PWD "\r\n"
	         "a=candidate:1 1 UDP 2122317823 127.0.0.1 %d typ host\r\n",
	         ntohs(peer.sin_port));
	if (juice_set_remote_description(agent, remote) < 0 || juice_gather_candidates(agent) < 0)
		return fail("cannot start ICE");
	for (int i = 0; i < 50 && !gathered; i++)
		usleep(100000);
	juice_get_local_description(agent, sdp, sizeof(sdp));
	int port = host_candidate_port(sdp);
	if (port <= 0)
		return fail("agent gathered no IPv4 host candidate");

	struct sockaddr_in target = {.sin_family = AF_INET,
	                             .sin_port = htons((uint16_t)port),
	                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	uint8_t txid[12];
	srand((unsigned)time(NULL));
	for (int i = 0; i < 12; i++)
		txid[i] = (uint8_t)rand();
	uint8_t request[256];
	size_t request_len = binding_request(request, txid);

	int result = -1;
	for (int attempt = 0; attempt < 5 && result < 0; attempt++) {
		sendto(sock, request, request_len, 0, (struct sockaddr *)&target, sizeof(target));
		uint8_t reply[1500];
		ssize_t n;
		while ((n = recv(sock, reply, sizeof(reply), 0)) >= 20) {
			if (memcmp(reply + 8, txid, 12) != 0)
				continue;
			uint16_t type = (uint16_t)(reply[0] << 8 | reply[1]);
			if (type == 0x0101) {
				result = 0;
			} else {
				fprintf(stderr, "libjuice zero tiebreaker regression: Binding request answered with type 0x%04x"
				                " (0x0111 is the 400 Bad Request this backport fixes)\n",
				        type);
				result = 1;
			}
			break;
		}
	}
	juice_destroy(agent);
	close(sock);
	if (result < 0)
		return fail("no response to the Binding request");
	if (!result)
		printf("libjuice accepts a zero ICE-CONTROLLED tiebreaker\n");
	return result;
}

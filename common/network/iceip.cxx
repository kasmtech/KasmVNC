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

#include <atomic>

#include <stdlib.h>

#include <network/StunServer.h>
#include <network/iceip.h>
#include <rfb/LogWriter.h>
#include <rfb/ServerCore.h>

static rfb::LogWriter vlog("ICE");

// Index of the built-in server that answered discovery; ICE skips the ones before it.
static std::atomic<size_t> reachableDefaultServer{0};

std::vector<std::string> iceStunUrls() {
	const std::string configured = static_cast<const char *>(rfb::Server::stunServer);
	if (network::usesDefaultStunServers(configured))
		return network::defaultIceStunUrls(reachableDefaultServer);
	if (const auto server = network::parseStunServer(configured))
		return {server->iceUrl()};

	vlog.error("Invalid StunServer '%s': WebRTC ICE will use no STUN servers", configured.c_str());
	return {};
}

static bool tryserver(const network::StunServer &server) {
	vlog.debug("Trying '%s', port %u", server.host.c_str(), server.port);

	// Standard says to wait up to 10s for a reply.
	const auto ip = network::queryStunMappedAddress(server, 10 * 1000);
	if (!ip)
		return false;

	rfb::Server::publicIP.setParam(ip->c_str());
	vlog.info("My public IP is %s", (const char *) rfb::Server::publicIP);
	return true;
}

void getPublicIP() {
	if (rfb::Server::publicIP[0]) {
		vlog.info("Using public IP %s from args",
				(const char *) rfb::Server::publicIP);
		return;
	}

	const std::string configured = static_cast<const char *>(rfb::Server::stunServer);
	std::optional<network::StunServer> custom;
	if (!network::usesDefaultStunServers(configured)) {
		custom = network::parseStunServer(configured);
		if (!custom) {
			vlog.error("Invalid StunServer '%s': expected hostname[:port] or stun:hostname[:port]",
				configured.c_str());
			exit(1);
		}
	}

	vlog.info("Querying public IP...");

	if (custom) {
		tryserver(*custom);
	} else {
		const auto &servers = network::defaultStunServers();
		for (size_t i = 0; i < servers.size(); i++) {
			if (tryserver(servers[i])) {
				reachableDefaultServer = i;
				break;
			}
			vlog.info("STUN server %zu didn't work, trying next...", i);
		}
	}

	if (!rfb::Server::publicIP[0]) {
		vlog.error("Failed to get public IP, please specify it with -publicIP");
		exit(1);
	}
}

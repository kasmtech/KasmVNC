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

#ifndef __NETWORK_UDP_H__
#define __NETWORK_UDP_H__

#include <stdint.h>
#include <rdr/OutStream.h>

void *udpserver(void *unused);
typedef struct WuClient WuClient;

namespace network {

	#define UDPSTREAM_BUFSIZE (1024 * 1024)

	class UdpStream: public rdr::OutStream {
		public:
			UdpStream();
			virtual void flush();
			virtual size_t length() { return total_len; }
			virtual void overrun(size_t needed);

			void setClient(WuClient *cli) {
				client = cli;
			}

			void setFrameNumber(const unsigned in) {
				frame = in;
			}

			bool isFailed() const;
			void clearFailed();
		private:
			uint8_t data[UDPSTREAM_BUFSIZE];
			WuClient *client;
			size_t total_len;
			uint32_t id;
			bool failed;
			uint32_t frame;
	};
}

extern "C" void wuGotHttp(const char msg[], const uint32_t msglen, char resp[]);


#endif // __NETWORK_UDP_H__

/* Copyright (C) 2021 Kasm.  All Rights Reserved.
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

#include <getopt.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>

static void help(const char name[]) {
	printf("Usage: %s [opts]\n\n"
		"-a --app-display disp	App display, default :0\n"
		"-v --vnc-display disp	VNC display, default :1\n"
		"\n"
		"-f --fps fps		FPS, default 30\n"
		"-r --resize		Enable resize, default disabled.\n"
		"			Do not enable this if there's a physical screen\n"
		"			connected to the app display.\n",
		name);
	exit(1);
}

int main(int argc, char **argv) {

	const char *appstr = ":0";
	const char *vncstr = ":1";
	uint8_t resize = 0;
	uint8_t fps = 30;

	const struct option longargs[] = {
		{"app-display", 1, NULL, 'a'},
		{"vnc-display", 1, NULL, 'v'},
		{"resize", 0, NULL, 'r'},
		{"fps", 1, NULL, 'f'},

		{NULL, 0, NULL, 0},
	};

	while (1) {
		int c = getopt_long(argc, argv, "a:v:rf:", longargs, NULL);
		if (c == -1)
			break;
		switch (c) {
			case 'a':
				appstr = strdup(optarg);
			break;
			case 'v':
				vncstr = strdup(optarg);
			break;
			case 'r':
				resize = 1;
			break;
			case 'f':
				fps = atoi(optarg);
				if (fps < 1 || fps > 120) {
					printf("Invalid fps\n");
					return 1;
				}
			break;
			default:
				help(argv[0]);
			break;
		}
	}

	return 0;
}

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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XTest.h>

#define min(a, b) ((a) < (b) ? (a) : (b))

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

	Display *appdisp = XOpenDisplay(appstr);
	if (!appdisp) {
		printf("Cannot open display %s\n", appstr);
		return 1;
	}
	if (!XShmQueryExtension(appdisp)) {
		printf("Display %s lacks SHM extension\n", appstr);
		return 1;
	}

	Display *vncdisp = XOpenDisplay(vncstr);
	if (!vncdisp) {
		printf("Cannot open display %s\n", vncstr);
		return 1;
	}
	if (!XShmQueryExtension(vncdisp)) {
		printf("Display %s lacks SHM extension\n", vncstr);
		return 1;
	}

	const int appscreen = DefaultScreen(appdisp);
	const int vncscreen = DefaultScreen(vncdisp);
	Visual *appvis = DefaultVisual(appdisp, appscreen);
	//Visual *vncvis = DefaultVisual(vncdisp, vncscreen);
	const int appdepth = DefaultDepth(appdisp, appscreen);
	const int vncdepth = DefaultDepth(vncdisp, vncscreen);
	if (appdepth != vncdepth) {
		printf("Depths don't match, app %u vnc %u\n", appdepth, vncdepth);
		return 1;
	}

	Window approot = DefaultRootWindow(appdisp);
	Window vncroot = DefaultRootWindow(vncdisp);
	XWindowAttributes appattr, vncattr;

	XGCValues gcval;
	gcval.plane_mask = AllPlanes;
	gcval.function = GXcopy;
	GC gc = XCreateGC(vncdisp, vncroot, GCFunction | GCPlaneMask, &gcval);

	XImage *img = NULL;
	XShmSegmentInfo shminfo;
	unsigned imgw = 0, imgh = 0;

	if (XGrabPointer(vncdisp, vncroot, False,
				ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
				GrabModeAsync, GrabModeAsync, None, None,
				CurrentTime) != Success)
		return 1;
	if (XGrabKeyboard(vncdisp, vncroot, False, GrabModeAsync, GrabModeAsync,
				CurrentTime) != Success)
		return 1;

	const unsigned sleeptime = 1000 * 1000 / fps;

	while (1) {
		if (!XGetWindowAttributes(appdisp, approot, &appattr))
			break;
		if (!XGetWindowAttributes(vncdisp, vncroot, &vncattr))
			break;
		if (resize && (appattr.width != vncattr.width ||
				appattr.height != vncattr.height)) {
			// resize app display to VNC display size
			XRRScreenConfiguration *config = XRRGetScreenInfo(appdisp, approot);

			int nsizes, i, match = -1;
			XRRScreenSize *sizes = XRRConfigSizes(config, &nsizes);
			//printf("%u sizes\n", nsizes);
			for (i = 0; i < nsizes; i++) {
				if (sizes[i].width == vncattr.width &&
					sizes[i].height == vncattr.height) {
					//printf("match %u\n", i);
					match = i;
					break;
				}
			}

			if (match >= 0) {
				XRRSetScreenConfig(appdisp, config, approot, match,
							RR_Rotate_0, CurrentTime);
			} else {
				/*XRRSetScreenSize(appdisp, approot,
						vncattr.width, vncattr.height,
						sizes[0].mwidth, sizes[0].mheight);*/
				XRRScreenResources *res = XRRGetScreenResources(appdisp, approot);

				char name[32];
				sprintf(name, "%ux%u_60", vncattr.width, vncattr.height);
				printf("Creating new Mode %s\n", name);
				XRRModeInfo *mode = XRRAllocModeInfo(name, strlen(name));

				mode->width = vncattr.width;
				mode->height = vncattr.height;

				RRMode rmode = XRRCreateMode(appdisp, approot, mode);
				XRRAddOutputMode(appdisp,
							res->outputs[0],
							rmode);
				XRRFreeModeInfo(mode);
				XRRFreeScreenResources(res);
			}

			XRRFreeScreenConfigInfo(config);
		}

		const unsigned w = min(appattr.width, vncattr.width);
		const unsigned h = min(appattr.height, vncattr.height);

		if (w != imgw || h != imgh) {
			if (img) {
				XShmDetach(appdisp, &shminfo);
				XDestroyImage(img);
				shmdt(shminfo.shmaddr);
				shmctl(shminfo.shmid, IPC_RMID, NULL);
			}
			img = XShmCreateImage(appdisp, appvis, appdepth, ZPixmap,
						NULL, &shminfo, w, h);
			if (!img)
				break;

			shminfo.shmid = shmget(IPC_PRIVATE,
						img->bytes_per_line * img->height,
						IPC_CREAT | 0666);
			if (shminfo.shmid == -1)
				break;
			shminfo.shmaddr = img->data = shmat(shminfo.shmid, 0, 0);
			shminfo.readOnly = False;
			if (!XShmAttach(appdisp, &shminfo))
				break;

			imgw = w;
			imgh = h;
		}

		XShmGetImage(appdisp, approot, img, 0, 0, 0xffffffff);
		XPutImage(vncdisp, vncroot, gc, img, 0, 0, 0, 0, w, h);

		// Handle events
		while (XPending(vncdisp)) {
			XEvent ev;
			XNextEvent(vncdisp, &ev);

			switch (ev.type) {
				case KeyPress:
				case KeyRelease:
					XTestFakeKeyEvent(appdisp, ev.xkey.keycode,
								ev.type == KeyPress,
								CurrentTime);
				break;
				case ButtonPress:
				case ButtonRelease:
					XTestFakeButtonEvent(appdisp, ev.xbutton.button,
								ev.type == ButtonPress,
								CurrentTime);
				break;
				case MotionNotify:
					XTestFakeMotionEvent(appdisp, appscreen,
								ev.xmotion.x,
								ev.xmotion.y,
								CurrentTime);
				break;
				default:
					printf("Unexpected event type %u\n", ev.type);
				break;
			}
		}

		usleep(sleeptime);
	}

	XCloseDisplay(appdisp);
	XCloseDisplay(vncdisp);

	return 0;
}

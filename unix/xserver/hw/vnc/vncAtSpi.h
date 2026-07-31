/* Copyright (C) 2026 Kasm.  All Rights Reserved.
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
 */

/* AT-SPI2 based focus tracking of editable text fields, used to drive
 * the client-side virtual keyboard on touch devices. Runs a dedicated
 * GLib thread; results are handed to the server thread through a pipe
 * (see vncAtSpiGetFd) plus vncAtSpiGetFocusState. */

#ifndef VNC_ATSPI_H
#define VNC_ATSPI_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int focused;                            /* editable text field has focus */
    int caretX, caretY, caretW, caretH;     /* caret rect, root coords */
    int fieldX, fieldY, fieldW, fieldH;     /* widget rect, root coords */
} VncAtSpiFocusInfo;

/* displayNum is the bare display number string (dix "display", e.g. "0").
 * Returns the read end of the notification pipe to watch, or -1. */
int vncAtSpiInit(const char *displayNum);

int vncAtSpiGetFd(void);

/* Stops the AT-SPI thread, deregisters listeners and calls atspi_exit().
 * Blocks up to a few seconds; safe to call if init never ran. */
void vncAtSpiShutdown(void);

/* Copies the latest focus state into out and clears the pending flag.
 * Returns 1 if there was an unread update, 0 otherwise. */
int vncAtSpiGetFocusState(VncAtSpiFocusInfo *out);

#ifdef __cplusplus
}
#endif

#endif /* VNC_ATSPI_H */

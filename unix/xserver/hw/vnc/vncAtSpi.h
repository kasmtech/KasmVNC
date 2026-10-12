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

#ifndef VNC_ATSPI_H
#define VNC_ATSPI_H

#include <rfb/touchTypes.h>

#ifdef __cplusplus
extern "C" {
#endif

int vncAtSpiInit(const char *displayNum);

int vncAtSpiGetFd(void);

void vncAtSpiRequestHitTest(int x, int y, unsigned int touchId);

void vncAtSpiShutdown(void);

int vncAtSpiGetFocusState(TapFocusInfo *out);

int vncAtSpiGetTextInputFields(TextInputFields *out);

#ifdef __cplusplus
}
#endif

#endif /* VNC_ATSPI_H */

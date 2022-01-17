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

#include "jsonescape.h"

void JSON_escape(const char *in, char *out) {
	for (; *in; in++) {
		if (in[0] == '\b') {
			*out++ = '\\';
			*out++ = 'b';
		} else if (in[0] == '\f') {
			*out++ = '\\';
			*out++ = 'f';
		} else if (in[0] == '\n') {
			*out++ = '\\';
			*out++ = 'n';
		} else if (in[0] == '\r') {
			*out++ = '\\';
			*out++ = 'r';
		} else if (in[0] == '\t') {
			*out++ = '\\';
			*out++ = 't';
		} else if (in[0] == '"') {
			*out++ = '\\';
			*out++ = '"';
		} else if (in[0] == '\\') {
			*out++ = '\\';
			*out++ = '\\';
		} else {
			*out++ = *in;
		}
	}

	*out = '\0';
}

void JSON_unescape(const char *in, char *out) {
	for (; *in; in++) {
		if (in[0] == '\\' && in[1] == 'b') {
			*out++ = '\b';
			in++;
		} else if (in[0] == '\\' && in[1] == 'f') {
			*out++ = '\f';
			in++;
		} else if (in[0] == '\\' && in[1] == 'n') {
			*out++ = '\n';
			in++;
		} else if (in[0] == '\\' && in[1] == 'r') {
			*out++ = '\r';
			in++;
		} else if (in[0] == '\\' && in[1] == 't') {
			*out++ = '\t';
			in++;
		} else if (in[0] == '\\' && in[1] == '"') {
			*out++ = '"';
			in++;
		} else if (in[0] == '\\' && in[1] == '\\') {
			*out++ = '\\';
			in++;
		} else {
			*out++ = *in;
		}
	}

	*out = '\0';
}

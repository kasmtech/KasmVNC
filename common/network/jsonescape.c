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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsonescape.h"
#include "cJSON.h"

void JSON_escape(const char *in, char *out) {
	if (!in)
		return;

	for (; *in; in++) {
		switch (*in) {
			case '\b':
				*out++ = '\\';
				*out++ = 'b';
				break;
			case '\f':
				*out++ = '\\';
				*out++ = 'f';
			case '\n':
				*out++ = '\\';
				*out++ = 'n';
				break;
			case '\r':
				*out++ = '\\';
				*out++ = 'r';
				break;
			case '\t':
				*out++ = '\\';
				*out++ = 't';
				break;
			case '"':
				*out++ = '\\';
				*out++ = '"';
				break;
			case '\\':
				*out++ = '\\';
				*out++ = '\\';
				break;
			default:
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

struct kasmpasswd_t *parseJsonUsers(const char *data) {

	cJSON *json = cJSON_Parse(data);
	if (!json)
		return NULL;

	if (!(json->type & cJSON_Array))
		return NULL;

	struct kasmpasswd_t *set = calloc(sizeof(struct kasmpasswd_t), 1);
	set->num = cJSON_GetArraySize(json);
	set->entries = calloc(sizeof(struct kasmpasswd_entry_t), set->num);

	cJSON *cur;
	unsigned s, len;
	for (cur = json->child, s = 0; cur; cur = cur->next, s++) {
		if (!(cur->type & cJSON_Object))
			goto fail;

		cJSON *e;
		struct kasmpasswd_entry_t * const entry = &set->entries[s];

		entry->user[0] = '\0';
		entry->password[0] = '\0';
		entry->write = entry->owner = 0;

		for (e = cur->child; e; e = e->next) {
			#define field(x) if (!strcmp(x, e->string))

			field("user") {
				if (!(e->type & cJSON_String))
					goto fail;
				len = strlen(e->valuestring);

				//printf("Val '%.*s'\n", len, start);

				if (len >= USERNAME_LEN)
					goto fail;

				memcpy(entry->user, e->valuestring, len);
				entry->user[len] = '\0';
			} else field("password") {
				if (!(e->type & cJSON_String))
					goto fail;
				len = strlen(e->valuestring);

				//printf("Val '%.*s'\n", len, start);

				if (len >= PASSWORD_LEN)
					goto fail;

				memcpy(entry->password, e->valuestring, len);
				entry->password[len] = '\0';
			} else field("write") {
				if (!(e->type & (cJSON_False | cJSON_True)))
					goto fail;

				if (e->type & cJSON_True)
					entry->write = 1;
			} else field("owner") {
				if (!(e->type & (cJSON_False | cJSON_True)))
					goto fail;

				if (e->type & cJSON_True)
					entry->owner = 1;
			} else field("read") {
				if (!(e->type & (cJSON_False | cJSON_True)))
					goto fail;

				if (e->type & cJSON_True)
					entry->read = 1;

			} else {
				//printf("Unknown field '%.*s'\n", len, start);
				goto fail;
			}

			#undef field
		}
	}

	cJSON_Delete(json);

	return set;
fail:
	free(set->entries);
	free(set);

	cJSON_Delete(json);

	return NULL;
}

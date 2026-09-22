#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/rand.h>
#include <crypt.h>

#include "kasmpasswd.h"

#define SALT_LEN 21

char *kasmpasswd_hash(const char *pass) {
	static const char valid[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	unsigned char raw[SALT_LEN - 5];
	char salt[SALT_LEN];

	if (!RAND_bytes(raw, sizeof(raw))) {
		fprintf(stderr, "Error: OpenSSL failed to generate a secure random salt.\n");
		exit(1);
	}

	salt[0] = '$';
	salt[1] = '5';
	salt[2] = '$';

	for (size_t i = 0; i < sizeof(raw); i++)
		salt[i + 3] = valid[raw[i] & 0x3f];

	salt[SALT_LEN - 2] = '$';
	salt[SALT_LEN - 1] = '\0';

	struct crypt_data cdata;
	cdata.initialized = 0;

	return crypt_r(pass, salt, &cdata);
}

int kasmpasswd_verify(const char *pass, const char *hash) {
	if (!pass || !hash)
		return 0;

	struct crypt_data cdata;
	cdata.initialized = 0;

	const char *result = crypt_r(pass, hash, &cdata);
	return result && !strcmp(result, hash);
}

struct kasmpasswd_t *readkasmpasswd(const char path[]) {

	struct kasmpasswd_t *set = calloc(sizeof(struct kasmpasswd_t), 1);
	FILE *f = fopen(path, "r");
	if (!f)
		return set;

	// Count lines
	unsigned lines = 0;
	char buf[4096];

	while (fgets(buf, 4096, f)) {
		lines++;
	}

	rewind(f);

	set->entries = calloc(sizeof(struct kasmpasswd_entry_t), lines);

	unsigned cur = 0;
	while (fgets(buf, 4096, f)) {
		char *lim = strchr(buf, ':');
		if (!lim)
			continue;
		*lim = '\0';
		lim++;

		const char * const pw = lim;

		lim = strchr(lim, ':');
		if (!lim)
			continue;
		*lim = '\0';
		lim++;

		const char * const perms = lim;

		lim = strchr(lim, '\n');
		if (lim)
			*lim = '\0';

		if (strlen(buf) + 1 > sizeof(((struct kasmpasswd_entry_t *)0)->user)) {
			fprintf(stderr, "Username %s too long\n", buf);
			continue;
		}
		if (strlen(pw) + 1 > sizeof(((struct kasmpasswd_entry_t *)0)->password)) {
			fprintf(stderr, "Password for user %s too long\n", buf);
			continue;
		}

		strcpy(set->entries[cur].user, buf);
		strcpy(set->entries[cur].password, pw);

		if (strchr(perms, 'r'))
			set->entries[cur].read = 1;
		if (strchr(perms, 'w'))
			set->entries[cur].write = 1;
		if (strchr(perms, 'o'))
			set->entries[cur].owner = 1;

		cur++;
	}

	fclose(f);

	set->num = cur;

	return set;
}

void writekasmpasswd(const char path[], const struct kasmpasswd_t *set) {
	char tmpname[PATH_MAX];

	if (!set || !set->entries || !set->num)
		return;

	snprintf(tmpname, PATH_MAX, "%s.tmp", path);
	tmpname[PATH_MAX - 1] = '\0';

	FILE *f = fopen(tmpname, "w");
	if (!f) {
		fprintf(stderr, "Failed to open temp file %s\n", tmpname);
		return;
	}

	unsigned i;
	for (i = 0; i < set->num; i++) {
		if (!set->entries[i].user[0])
			continue;

		fprintf(f, "%s:%s:%s%s%s\n",
			set->entries[i].user,
			set->entries[i].password,
			set->entries[i].read ? "r" : "",
			set->entries[i].write ? "w" : "",
			set->entries[i].owner ? "o" : "");
	}

	fsync(fileno(f));
	fclose(f);

	if (rename(tmpname, path))
		fprintf(stderr, "Failed writing the password file %s\n", path);
	chmod(path, S_IRUSR|S_IWUSR);
}

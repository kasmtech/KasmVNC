#ifndef KASMPASSWD_H
#define KASMPASSWD_H

#ifdef __cplusplus
extern "C" {
#endif

struct kasmpasswd_entry_t {
	char user[32];
	char password[128];
	unsigned char write : 1;
	unsigned char owner : 1;
};

struct kasmpasswd_t {
	struct kasmpasswd_entry_t *entries;
	unsigned num;
};

struct kasmpasswd_t *readkasmpasswd(const char path[]);
void writekasmpasswd(const char path[], const struct kasmpasswd_t *set);

#ifdef __cplusplus
} // extern C
#endif

#endif

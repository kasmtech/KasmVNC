/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright (C) 2010 Antoine Martin.  All Rights Reserved.
 * Copyright (C) 2010 D. R. Commander.  All Rights Reserved.
 * Copyright (C) 2020 Kasm.  All Rights Reserved.
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

#include <crypt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <wordexp.h>

#include "kasmpasswd.h"

static void usage(const char *prog)
{
  fprintf(stderr, "Usage: %s -u username [-wnod] [password_file]\n"
                  "-r	Read permission\n"
                  "-w	Write permission\n"
                  "-o	Owner\n"
                  "-n	Don't change password, change permissions only\n"
                  "-d	Delete this user\n"
                  "\n"
                  "The password file is updated atomically.\n"
                  "For more information, run \"man vncpasswd\".\n\n"
                  "To pass the password via a pipe, use\n"
                  "echo -e \"password\\npassword\\n\" | %s [-args]\n",
                  prog, prog);
  exit(1);
}


static void enableEcho(unsigned char enable) {
  struct termios attrs;

  if (!isatty(fileno(stdin)))
    return;

  tcgetattr(fileno(stdin), &attrs);
  if (enable)
    attrs.c_lflag |= ECHO;
  else
    attrs.c_lflag &= ~ECHO;
  attrs.c_lflag |= ECHONL;
  tcsetattr(fileno(stdin), TCSAFLUSH, &attrs);
}

static const char *encryptpw(const char *in) {
  return crypt(in, "$5$kasm$");
}

static char* getpassword(const char* prompt, char *buf) {
  if (prompt && isatty(fileno(stdin))) fputs(prompt, stdout);
  enableEcho(0);
  char* result = fgets(buf, 4096, stdin);
  enableEcho(1);
  if (result) {
    if (result[strlen(result)-1] == '\n')
      result[strlen(result)-1] = 0;
    return buf;
  }
  return NULL;
}

static char pw1[4096];
static char pw2[4096];

static const char *readpassword() {
  while (1) {
    if (!getpassword("Password:", pw1)) {
      perror("getpassword error");
      exit(1);
    }
    if (strlen(pw1) < 6) {
      if (strlen(pw1) == 0) {
        fprintf(stderr,"Password not changed\n");
        exit(1);
      }
      fprintf(stderr,"Password must be at least 6 characters - try again\n");
      continue;
    }

    if (!getpassword("Verify:", pw2)) {
      perror("getpass error");
      exit(1);
    }
    if (strcmp(pw1, pw2) != 0) {
      if (!isatty(fileno(stdin))) {
        fprintf(stderr,"Passwords don't match\n");
        exit(1);
      }
      fprintf(stderr,"Passwords don't match - try again\n");
      continue;
    }

    return encryptpw(pw1);
  }
}

int main(int argc, char** argv)
{
  const char *fname = NULL;
  const char *user = NULL;
  const char args[] = "u:rwnod";
  int opt;

  unsigned char nopass = 0, reader = 0, writer = 0, owner = 0, deleting = 0;

  while ((opt = getopt(argc, argv, args)) != -1) {
    switch (opt) {
      case 'u':
        user = optarg;
        if (strlen(user) + 1 > sizeof(((struct kasmpasswd_entry_t *)0)->user)) {
          fprintf(stderr, "Username %s too long\n", user);
          exit(1);
        }
      break;
      case 'n':
        nopass = 1;
      break;
      case 'r':
        reader = 1;
      break;
      case 'w':
        writer = 1;
      break;
      case 'o':
        owner = 1;
      break;
      case 'd':
        deleting = 1;
      break;
      default:
        usage(argv[0]);
      break;
    }
  }

  if (deleting && (nopass || reader || writer || owner))
    usage(argv[0]);

  if (!user)
    usage(argv[0]);

  if (optind < argc)
    fname = argv[optind];

  if (!fname) {
    wordexp_t wexp;
    if (!wordexp("~/.kasmpasswd", &wexp, WRDE_NOCMD) && wexp.we_wordv[0])
      fname = strdup(wexp.we_wordv[0]);
    wordfree(&wexp);
  }
  if (!fname)
    usage(argv[0]);

  // Action
  struct kasmpasswd_t *set = readkasmpasswd(fname);
  unsigned i;

  if (nopass) {
    for (i = 0; i < set->num; i++) {
      if (!strcmp(set->entries[i].user, user)) {
        set->entries[i].read = reader;
        set->entries[i].write = writer;
        set->entries[i].owner = owner;

        writekasmpasswd(fname, set);
        return 0;
      }
    }
    fprintf(stderr, "No user named %s found\n", user);
    return 1;

  } else if (deleting) {
    for (i = 0; i < set->num; i++) {
      if (!strcmp(set->entries[i].user, user)) {
        set->entries[i].user[0] = '\0';

        writekasmpasswd(fname, set);
        return 0;
      }
    }
    fprintf(stderr, "No user named %s found\n", user);
    return 1;
  } else {
    const char *encrypted = readpassword();
    for (i = 0; i < set->num; i++) {
      if (!strcmp(set->entries[i].user, user))
        break;
    }

    // No existing user by that name?
    if (i >= set->num) {
      i = set->num++;
      set->entries = realloc(set->entries, set->num * sizeof(struct kasmpasswd_entry_t));
    }

    strcpy(set->entries[i].user, user);
    strcpy(set->entries[i].password, encrypted);
    set->entries[i].read = reader;
    set->entries[i].write = writer;
    set->entries[i].owner = owner;

    writekasmpasswd(fname, set);
  }

  return 0;
}

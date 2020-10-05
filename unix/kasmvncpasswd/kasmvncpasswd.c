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

static void usage(const char *prog)
{
  fprintf(stderr, "Usage: %s [file]\n", prog);
  fprintf(stderr, "       %s -f\n", prog);
  exit(1);
}


static void enableEcho(unsigned char enable) {
  struct termios attrs;
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
  if (prompt) fputs(prompt, stdout);
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

// Reads passwords from stdin and prints encrypted passwords to stdout.
static int encrypt_pipe() {
  char *result = getpassword(NULL, pw1);
  if (!result)
    return 1;

  printf("%s", encryptpw(result));
  fflush(stdout);

  return 0;
}

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
      fprintf(stderr,"Passwords don't match - try again\n");
      continue;
    }

    return encryptpw(pw1);
  }
}

int main(int argc, char** argv)
{
  char* fname = 0;

  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "-f", 2) == 0) {
      return encrypt_pipe();
    } else if (argv[i][0] == '-') {
      usage(argv[0]);
    } else if (!fname) {
      fname = argv[i];
    } else {
      usage(argv[0]);
    }
  }

  if (!fname) {
    wordexp_t wexp;
    if (!wordexp("~/.kasmpasswd", &wexp, WRDE_NOCMD) && wexp.we_wordv[0])
      fname = strdup(wexp.we_wordv[0]);
    wordfree(&wexp);
  }
  if (!fname)
    usage(argv[0]);

  while (1) {
    const char *encrypted = readpassword();

    FILE* fp = fopen(fname, "w");
    if (!fp) {
      fprintf(stderr, "Couldn't open %s for writing\n", fname);
      exit(1);
    }
    chmod(fname, S_IRUSR|S_IWUSR);

    if (fwrite(encrypted, strlen(encrypted), 1, fp) != 1) {
      fprintf(stderr,"Writing to %s failed\n",fname);
      exit(1);
    }

    fclose(fp);

    return 0;
  }
}

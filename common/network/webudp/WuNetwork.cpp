#include "WuNetwork.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void HexDump(const uint8_t* src, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (i % 8 == 0) printf("%04x ", uint32_t(i));

    printf("%02x ", src[i]);

    if ((i + 1) % 8 == 0) printf("\n");
  }
  printf("\n");
}

int MakeNonBlocking(int sfd) {
  int flags = fcntl(sfd, F_GETFL, 0);
  if (flags == -1) {
    return -1;
  }

  flags |= O_NONBLOCK;

  int s = fcntl(sfd, F_SETFL, flags);
  if (s == -1) {
    return -1;
  }

  return 0;
}

int CreateSocket(uint16_t port) {

  int sfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sfd == -1) {
    return -1;
  }

  int enable = 1;
  setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == 0)
    return sfd;

  close(sfd);

  return -1;
}

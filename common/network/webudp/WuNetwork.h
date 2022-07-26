#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

void HexDump(const uint8_t* src, size_t len);
int MakeNonBlocking(int sfd);
int CreateSocket(uint16_t port);

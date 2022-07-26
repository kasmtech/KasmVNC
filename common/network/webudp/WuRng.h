#pragma once

#include <stddef.h>
#include <stdint.h>

// http://xoroshiro.di.unimi.it/xoroshiro128plus.c
struct WuRngState {
  uint64_t s[2];
};

uint64_t WuRandomU64();
uint32_t WuRandomU32();

void WuRandomString(char* out, size_t length);

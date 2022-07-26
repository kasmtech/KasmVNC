#include <stdlib.h>
#include "WuRng.h"

static const char kCharacterTable[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static inline uint64_t rotl(const uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}

static uint64_t WuGetRngSeed() {
  uint64_t x = rand();
  uint64_t z = (x += UINT64_C(0x9E3779B97F4A7C15));
  z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
  return z ^ (z >> 31);
}

static void WuRngInit(WuRngState* state, uint64_t seed) {
  state->s[0] = seed;
  state->s[1] = seed;
}

static uint64_t WuRngNext(WuRngState* state) {
  const uint64_t s0 = state->s[0];
  uint64_t s1 = state->s[1];
  const uint64_t result = s0 + s1;

  s1 ^= s0;
  state->s[0] = rotl(s0, 55) ^ s1 ^ (s1 << 14);
  state->s[1] = rotl(s1, 36);

  return result;
}

void WuRandomString(char* out, size_t length) {
  WuRngState state;
  WuRngInit(&state, WuGetRngSeed());

  for (size_t i = 0; i < length; i++) {
    out[i] = kCharacterTable[WuRngNext(&state) % (sizeof(kCharacterTable) - 1)];
  }
}

uint64_t WuRandomU64() {
  WuRngState state;
  WuRngInit(&state, WuGetRngSeed());
  return WuRngNext(&state);
}

uint32_t WuRandomU32() { return (uint32_t)WuRandomU64(); }

#pragma once

#include <stdint.h>

struct WuArena {
  uint8_t* memory;
  int32_t length;
  int32_t capacity;
};

void WuArenaInit(WuArena* arena, int32_t capacity);
void* WuArenaAcquire(WuArena* arena, int32_t blockSize);
void WuArenaReset(WuArena* arena);
void WuArenaDestroy(WuArena* arena);

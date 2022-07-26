#include "WuArena.h"
#include <assert.h>
#include <stdlib.h>

void WuArenaInit(WuArena* arena, int32_t capacity) {
  arena->memory = (uint8_t*)calloc(capacity, 1);
  arena->length = 0;
  arena->capacity = capacity;
}

void* WuArenaAcquire(WuArena* arena, int32_t blockSize) {
  assert(blockSize > 0);
  int32_t remain = arena->capacity - arena->length;

  if (remain >= blockSize) {
    uint8_t* m = arena->memory + arena->length;
    arena->length += blockSize;
    return m;
  }

  return NULL;
}

void WuArenaReset(WuArena* arena) { arena->length = 0; }

void WuArenaDestroy(WuArena* arena) { free(arena->memory); }

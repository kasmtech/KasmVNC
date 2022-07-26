#include "WuString.h"
#include <ctype.h>
#include <stdint.h>
#include <string.h>

int32_t FindTokenIndex(const char* s, size_t len, char token) {
  for (size_t i = 0; i < len; i++) {
    if (s[i] == token) return i;
  }

  return -1;
}

bool MemEqual(const void* first, size_t firstLen, const void* second,
              size_t secondLen) {
  if (firstLen != secondLen) return false;

  return memcmp(first, second, firstLen) == 0;
}

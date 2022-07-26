#include <stddef.h>
#include <stdint.h>

int32_t FindTokenIndex(const char* s, size_t len, char token);
bool MemEqual(const void* first, size_t firstLen, const void* second,
              size_t secondLen);

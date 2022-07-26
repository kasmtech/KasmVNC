#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

template <typename T>
T ByteSwap(T v) {
  if (sizeof(T) == 1) {
    return v;
  } else if (sizeof(T) == 2) {
    return __builtin_bswap16(uint16_t(v));
  } else if (sizeof(T) == 4) {
    return __builtin_bswap32(uint32_t(v));
  } else if (sizeof(T) == 8) {
    return __builtin_bswap64(uint64_t(v));
  } else {
    assert(0);
    return 0;
  }
}

template <typename T>
size_t WriteScalar(uint8_t* dest, T v) {
  *((T*)dest) = v;
  return sizeof(T);
}

template <typename T>
int32_t ReadScalar(const uint8_t* src, T* v) {
  *v = *(const T*)src;
  return sizeof(T);
}

template <typename T>
size_t WriteScalarSwapped(uint8_t* dest, T v) {
  *((T*)dest) = ByteSwap(v);
  return sizeof(T);
}

template <typename T>
int32_t ReadScalarSwapped(const uint8_t* src, T* v) {
  *v = ByteSwap(*(const T*)src);
  return sizeof(T);
}

inline int32_t PadSize(int32_t numBytes, int32_t alignBytes) {
  return ((numBytes + alignBytes - 1) & ~(alignBytes - 1)) - numBytes;
}

#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif
#include <stdint.h>

inline int64_t HpCounter() {
#ifdef _WIN32
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  int64_t i64 = li.QuadPart;
#else
  struct timeval t;
  gettimeofday(&t, 0);
  int64_t i64 = t.tv_sec * int64_t(1000000) + t.tv_usec;
#endif
  return i64;
}

inline int64_t HpFreq() {
#ifdef _WIN32
  LARGE_INTEGER li;
  QueryPerformanceFrequency(&li);
  return li.QuadPart;
#else
  return int64_t(1000000);
#endif
}

inline double MsNow() {
  return double(HpCounter()) * 1000.0 / double(HpFreq());
}

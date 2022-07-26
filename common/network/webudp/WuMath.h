#pragma once

template <typename T>
const T& Min(const T& a, const T& b) {
  if (a < b) return a;

  return b;
}

template <typename T>
const T& Max(const T& a, const T& b) {
  if (a > b) return a;

  return b;
}

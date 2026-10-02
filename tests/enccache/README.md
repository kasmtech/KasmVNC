# EncCache regression tests

Run inside a Linux development container with a C++20 compiler and CMake:

```sh
cmake -S tests/enccache -B /tmp/enccache-build -DENABLE_ASAN=ON
cmake --build /tmp/enccache-build
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir /tmp/enccache-build --output-on-failure
```

This standalone target compiles the actual `common/rfb/EncCache.cxx` without
server dependencies or the legacy viewer benchmarks. Tests exit nonzero on
failed behavior checks. AddressSanitizer/LeakSanitizer are required to check
that replacement, clear, and destruction release their allocations and that
re-adding the same pointer does not free it prematurely.

Coverage includes lexicographic ordering across all five key fields, five
frames of 500 distinct rectangles looked up by a second viewer, repeated
clear and missing keys, replacement data and length, same-pointer replacement,
and destruction without explicit clear. These are cache-level tests, not a
full Xkasmvnc build or a multi-viewer runtime benchmark.

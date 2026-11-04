# OpenSSL certificate and DTLS regression tests

Run this standalone suite inside a build/test container with a C++20 compiler and
OpenSSL development libraries. It compiles the production WebUDP code without an
X server or GPU. Do not build or run it directly on the host.

```sh
builder/openssl-tests/build /tmp/openssl-tests
/tmp/openssl-tests/crypto-regression
```

`CPPFLAGS`, `CXXFLAGS`, `LDFLAGS`, and `CXX` may select a portable toolchain and
OpenSSL 3.0 headers/libraries. Keep the resulting executable and run the same
binary in target distro containers to check ABI compatibility. The binary should
require `libssl.so.3` and `libcrypto.so.3`, without OpenSSL version requirements
newer than `OPENSSL_3.0.0`. Run on at least Ubuntu 22.04 and an EL9 system using
its unmodified default crypto policy. EL9 rejects the old SHA1 certificate
signature even though creating a DTLS context alone could report success.

The tests check RSA key size, SHA256 signing and fingerprint agreement, a
complete DTLS 1.2 handshake and encrypted payload exchange using the production
server context and client, and initialization failure propagation. GNU linker wrappers
inject key/context/certificate allocation, key generation, signing, and digest
failures. The partial-key and repeated-initialization cases are also suitable
for AddressSanitizer/LeakSanitizer runs. Live-client teardown and memory-pool
alignment/reuse are covered as well; production builds have no injection code.

```sh
CXXFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
LDFLAGS='-fsanitize=address,undefined' \
  builder/openssl-tests/build /tmp/openssl-tests-asan
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/openssl-tests-asan/crypto-regression
```

The in-memory DTLS exchange tests the production certificate, SSL context, and
client initialization.
Browser ICE/STUN/SCTP data-channel negotiation and packaged HTTPS/WebSocket
behavior require the separate runtime and browser functional tests.

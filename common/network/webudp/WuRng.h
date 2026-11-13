#pragma once

#include <stddef.h>
#include <stdint.h>

// Cryptographically secure (OpenSSL RAND_bytes); aborts if the CSPRNG fails.
uint64_t WuRandomU64();
uint32_t WuRandomU32();

void WuRandomString(char* out, size_t length);

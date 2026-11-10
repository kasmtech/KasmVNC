#include <stdlib.h>
#include <openssl/rand.h>
#include "WuRng.h"

static const char kCharacterTable[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void WuRandomBytes(void *out, size_t length) {
    if (RAND_bytes(static_cast<unsigned char *>(out), static_cast<int>(length)) != 1)
        abort();
}

void WuRandomString(char *out, size_t length) {
    static constexpr unsigned table_len = sizeof(kCharacterTable) - 1;
    static constexpr unsigned limit = 256 - 256 % table_len;

    size_t i = 0;
    while (i < length) {
        unsigned char buf[64];
        constexpr size_t size = sizeof(buf);

        WuRandomBytes(buf, size);

        for (size_t j = 0; j < size && i < length; j++) {
            if (buf[j] < limit)
                out[i++] = kCharacterTable[buf[j] % table_len];
        }
    }
}

uint64_t WuRandomU64() {
    uint64_t v;
    WuRandomBytes(&v, sizeof(v));
    return v;
}

uint32_t WuRandomU32() {
    uint32_t v;
    WuRandomBytes(&v, sizeof(v));
    return v;
}

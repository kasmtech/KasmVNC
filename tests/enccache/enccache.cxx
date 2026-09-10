/* Regression tests for the encoded rectangle cache.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#include <rfb/EncCache.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
        std::exit(1); \
    } \
} while (0)

static rfb::EncId key(unsigned i) {
    rfb::EncId id{};
    id.type = 7;
    id.x = (i * 73) % 1200;
    id.y = (i * 31) % 900;
    id.w = 10 + i % 240;
    id.h = 10 + i % 190;
    return id;
}

static void *buffer(unsigned size, unsigned char value) {
    void *data = std::malloc(size);
    CHECK(data != NULL);
    std::memset(data, value, size);
    return data;
}

static void add(rfb::EncCache& cache, const rfb::EncId& id,
                unsigned size, void *data) {
    cache.add(id.type, id.x, id.y, id.w, id.h, size, data);
}

static const void *get(const rfb::EncCache& cache, const rfb::EncId& id,
                       uint32_t& size) {
    return cache.get(id.type, id.x, id.y, id.w, id.h, size);
}

static void ordering() {
    // Every combination of two values for each of the five identity fields.
    std::vector<rfb::EncId> ids;
    for (unsigned i = 0; i < 32; ++i) {
        rfb::EncId id{};
        id.type = (i >> 4) & 1;
        id.x = (i >> 3) & 1;
        id.y = (i >> 2) & 1;
        id.w = 1 + ((i >> 1) & 1);
        id.h = 1 + (i & 1);
        ids.push_back(id);
    }
    for (unsigned i = 0; i < ids.size(); ++i) {
        CHECK(!(ids[i] < ids[i]));
        for (unsigned j = 0; j < ids.size(); ++j) {
            CHECK((ids[i] < ids[j]) == (i < j));
            CHECK(!(ids[i] < ids[j] && ids[j] < ids[i]));
            for (unsigned k = 0; k < ids.size(); ++k) {
                if (ids[i] < ids[j] && ids[j] < ids[k])
                    CHECK(ids[i] < ids[k]);
            }
        }
    }
}

static void lookup() {
    rfb::EncCache cache;
    for (unsigned frame = 0; frame < 5; ++frame) {
        for (unsigned i = 0; i < 500; ++i)
            add(cache, key(i), 1024, buffer(1024, i % 256));
        // The second viewer must find every rectangle from the first viewer.
        for (unsigned i = 0; i < 500; ++i) {
            uint32_t size = 0;
            const void *data = get(cache, key(i), size);
            CHECK(data != NULL);
            CHECK(size == 1024);
            const unsigned char *bytes = static_cast<const unsigned char *>(data);
            for (unsigned j = 0; j < size; ++j)
                CHECK(bytes[j] == i % 256);
        }
        cache.clear();
        cache.clear();
        for (unsigned i = 0; i < 500; ++i) {
            uint32_t size = 123;
            CHECK(get(cache, key(i), size) == NULL);
            CHECK(size == 123);
        }
    }
}

static void replacement() {
    rfb::EncCache cache;
    const rfb::EncId id = key(1);
    add(cache, id, 16, buffer(16, 0x11));
    void *replacement = buffer(4096, 0x22);
    add(cache, id, 4096, replacement);
    uint32_t size = 0;
    CHECK(get(cache, id, size) == replacement);
    CHECK(size == 4096);
    CHECK(static_cast<const unsigned char *>(replacement)[size - 1] == 0x22);
    // Re-adding the same allocation must update its length without freeing it.
    add(cache, id, 32, replacement);
    CHECK(get(cache, id, size) == replacement);
    CHECK(size == 32);
    CHECK(static_cast<const unsigned char *>(replacement)[size - 1] == 0x22);
    cache.clear();
    CHECK(get(cache, id, size) == NULL);
}

static void destruction() {
    // LeakSanitizer checks ownership on scope exit without an explicit clear().
    for (unsigned i = 0; i < 20; ++i) {
        rfb::EncCache cache;
        add(cache, key(i), 4096, buffer(4096, i));
        uint32_t size = 0;
        CHECK(get(cache, key(i), size) != NULL);
        CHECK(size == 4096);
    }
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (std::strcmp(argv[1], "ordering") == 0)
        ordering();
    else if (std::strcmp(argv[1], "lookup") == 0)
        lookup();
    else if (std::strcmp(argv[1], "replacement") == 0)
        replacement();
    else if (std::strcmp(argv[1], "destruction") == 0)
        destruction();
    else
        return 1;
    std::printf("%s: OK\n", argv[1]);
    return 0;
}

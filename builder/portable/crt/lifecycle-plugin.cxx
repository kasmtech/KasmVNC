// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
static void event(const char *text)
{
    assert(write(STDOUT_FILENO, text, strlen(text)) == (ssize_t)strlen(text));
    assert(write(STDOUT_FILENO, "\n", 1) == 1);
}
static void exit_handler() { event("plugin:atexit"); }
struct Object {
    Object() { event("plugin:constructor"); assert(atexit(exit_handler) == 0); }
    ~Object() { event("plugin:destructor"); }
} object;
extern "C" int plugin_probe() { event("plugin:call"); return 91; }
__attribute__((destructor)) static void destructor() { event("plugin:attribute-destructor"); }

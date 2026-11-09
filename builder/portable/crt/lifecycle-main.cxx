// SPDX-License-Identifier: GPL-2.0-or-later
// End-to-end fixture for the portable process-entry ABI, executed by test-crt.py.
#include <cassert>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>

extern char **environ;
static void event(const char *text)
{
    assert(write(STDOUT_FILENO, text, strlen(text)) == (ssize_t)strlen(text));
    assert(write(STDOUT_FILENO, "\n", 1) == 1);
}
static void first_exit() { event("atexit:first"); }
static void second_exit() { event("atexit:second"); }
struct Global {
    Global() { event("global:constructor"); }
    ~Global() { event("global:destructor"); }
} global;
__attribute__((constructor)) static void constructor(int argc, char **argv, char **envp)
{
    assert(argc == 4 && argv && envp && envp == environ);
    assert(!strcmp(argv[1], "first-argument"));
    assert(!strcmp(getenv("KASM_CRT_TEST_ENV"), "environment-value"));
    event("attribute:constructor");
}
__attribute__((destructor)) static void destructor() { event("attribute:destructor"); }
struct Local {
    int value = 23;
    ~Local() { event("tls:destructor"); }
};
thread_local Local local;
static volatile sig_atomic_t received_signal;
static void signal_handler(int signal) { received_signal = signal; }

static void preinit(int argc, char **argv, char **envp)
{
    assert(argc == 4 && argv && envp && !strcmp(argv[1], "first-argument"));
    event("preinit");
}
__attribute__((section(".preinit_array"), used))
static void (*preinit_slot)(int, char **, char **) = preinit;

int main(int argc, char **argv, char **envp)
{
    assert(argc == 4 && !strcmp(argv[1], "first-argument") && !strcmp(argv[2], "second-argument"));
    assert(envp == environ && !strcmp(getenv("KASM_CRT_TEST_ENV"), "environment-value"));
    event("main:arguments-environment");
    assert(atexit(first_exit) == 0 && atexit(second_exit) == 0);
    assert(local.value == 23);
    local.value = 77;
    std::thread worker([] {
        assert(local.value == 23);
        local.value = 42;
        event("thread:tls");
    });
    worker.join();
    assert(local.value == 77);
    try {
        throw std::runtime_error("caught-value");
    } catch (const std::runtime_error &error) {
        assert(!strcmp(error.what(), "caught-value"));
        event("exception:caught");
    }
    assert(signal(SIGUSR1, signal_handler) != SIG_ERR);
    assert(raise(SIGUSR1) == 0 && received_signal == SIGUSR1);
    event("signal:handled");
    void *plugin = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
    assert(plugin);
    auto function = reinterpret_cast<int (*)()>(dlsym(plugin, "plugin_probe"));
    assert(function && function() == 91);
    assert(dlclose(plugin) == 0);
    event("main:return");
    return 17;
}

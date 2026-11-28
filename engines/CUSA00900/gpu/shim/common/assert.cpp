// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#include <vector>

/// XStation 2026-11-16 (TLOU 2026-11-09): this thread translates shaders with no game behind them (the cache
/// recipe). A failed assert fails that one shader instead of ending the program.
thread_local bool g_shader_prebuild_test = false;
/// What this thread reads for descriptors and the fetch shader instead of guest memory.
thread_local const std::vector<unsigned>* g_shader_fake_flat = nullptr;
thread_local const void* g_shader_fake_fetch = nullptr;

void assert_fail_impl() {
    if (g_shader_prebuild_test) {
        throw std::runtime_error("assert while translating a shader with no game");
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}

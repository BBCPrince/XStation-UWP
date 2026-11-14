// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include "common/assert.h"
#include "common/logging/log.h"

/// TLOU test (BB_SHADER_PACK, 2026-11-09): this thread translates shaders with no game state
/// behind them. A failed assert fails that one shader instead of ending the program.
thread_local bool g_shader_prebuild_test = false;
/// TLOU test (BB_SHADER_REPLAY): what this thread reads for descriptors and the fetch shader.
thread_local const std::vector<unsigned>* g_shader_fake_flat = nullptr;
thread_local const void* g_shader_fake_fetch = nullptr;
namespace Shader { struct Info; }
/// TLOU pre-build: fills in descriptors once a shader's resources are known.
thread_local void (*g_shader_fake_sharps)(Shader::Info&) = nullptr;

void assert_fail_impl() {
    if (g_shader_prebuild_test) {
        throw std::runtime_error("assert while translating a shader for the prebuild test");
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

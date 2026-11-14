// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/logging/log.h"

// A draw-preparation worker translates shaders and builds pipelines ahead of the GPU thread, from
// guest memory the game may already have reused. Where it armed a recovery point (the same one a
// faulting read uses), an assertion on such data only abandons the speculation; the GPU thread
// prepares the draw itself, from memory as it is when the draw executes.
static void abandon_speculation() {
    if (!runtime_fault_recover) {
        return;
    }
    sigjmp_buf* recover = runtime_fault_recover;
    runtime_fault_recover = nullptr;
    std::fputs("GPU assertion while preparing a draw ahead of the GPU thread: leaving the draw to the GPU thread\n",
               stderr);
#ifdef _WIN32
    bb_longjmp(*recover, 1);
#else
    siglongjmp(*recover, 1);
#endif
}

void assert_fail_impl() {
    abandon_speculation();
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
#ifdef _WIN32
    // Uncharted Xbox port (2026-11-04): the call stack (module+offset) of the failed assertion.
    void* frames[32];
    const USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<const char*>(frames[i]), &module);
        if (module) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base = std::strrchr(name, '\\');
        std::fprintf(stderr, "  assert frame %u %s+0x%llx\n", unsigned(i), base ? base + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
#endif
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

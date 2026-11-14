// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <vector>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

#include "common/assert.h"

// tlou194_uwp_memory.c: writes the pager/renderer event ring out (pager-gpu-events-N.txt)
extern "C" void runtime_trace_dump(const char* why);

namespace Vulkan {
void BbSubmitRingDump(u64 gpu_tick, u64 waited_tick); // vk_rasterizer.cpp

constexpr u64 kWaitTimeout = std::numeric_limits<u64>::max();

namespace {

// bbport: GPU stall report. TLOU on the Xbox (2026-11-02) froze with the GPU 100% busy and
// nothing in the log said where. When a timeline has had work queued without progress for
// BB_GPU_WATCHDOG_S seconds (default 10; 0 off; a tick taken but not yet submitted counts),
// every timeline's queued and finished ticks are printed once.
std::mutex registry_mutex;
std::vector<Semaphore*> registry;

void Watchdog(const Instance* instance, int limit_s) {
    struct Seen {
        Semaphore* semaphore;
        u64 counter;
        std::chrono::steady_clock::time_point since;
    };
    std::vector<Seen> seen;
    // BB_GPU_HANG_TEST=N: reports as hung N seconds after start (checks the report itself).
    const char* test_env = std::getenv("BB_GPU_HANG_TEST");
    const int test_s = test_env ? std::atoi(test_env) : 0;
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{registry_mutex};
        bool hung = test_s > 0 && now - started >= std::chrono::seconds(test_s);
        std::vector<Seen> next;
        for (Semaphore* semaphore : registry) {
            auto [result, counter] = instance->GetDevice().getSemaphoreCounterValue(semaphore->Handle());
            if (result != vk::Result::eSuccess) {
                continue;
            }
            auto since = now;
            for (const Seen& old : seen) {
                if (old.semaphore == semaphore && old.counter == counter) {
                    since = old.since;
                }
            }
            const u64 queued = semaphore->CurrentTick() - 1;
            if (queued <= counter) {
                since = now; // nothing waiting on the GPU
            } else if (now - since >= std::chrono::seconds(limit_s)) {
                hung = true;
            }
            next.push_back({semaphore, counter, since});
        }
        seen = std::move(next);
        if (!hung) {
            continue;
        }
        std::printf("GPU: stall: no progress for %d s with work queued\n", limit_s);
        for (const Seen& s : seen) {
            const u64 queued = s.semaphore->CurrentTick() - 1;
            std::printf("GPU: stall: timeline %s (handle %llx): queued %llu, finished %llu (%llu behind, stuck %.0f s)\n",
                        s.semaphore->Name(),
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(static_cast<VkSemaphore>(s.semaphore->Handle()))),
                        static_cast<unsigned long long>(queued),
                        static_cast<unsigned long long>(s.counter),
                        static_cast<unsigned long long>(queued > s.counter ? queued - s.counter : 0),
                        std::chrono::duration<double>(now - s.since).count());
        }
        // DRED, which would say where each queue stopped, is unsupported on the Xbox (its data
        // calls return DXGI_ERROR_UNSUPPORTED even when enabled), so the device is left alone.
        std::fflush(stdout);
        runtime_trace_dump("GPU stall"); // registry_mutex is held: the handles are in the lines above

        return;
    }
}

} // namespace

// The event ring's gpu-submit lines name a timeline by its handle: this says which is which.
void LogTimelineHandles() {
    std::scoped_lock lock{registry_mutex};
    for (Semaphore* semaphore : registry) {
        std::printf("GPU: timeline %s is handle %llx, queued %llu\n", semaphore->Name(),
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(static_cast<VkSemaphore>(semaphore->Handle()))),
                    static_cast<unsigned long long>(semaphore->CurrentTick() - 1));
    }
    std::fflush(stdout);
}

Semaphore::Semaphore(const Instance& instance_) : instance{instance_} {
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
    {
        std::scoped_lock lock{registry_mutex};
        registry.push_back(this);
    }
    static const bool watchdog = [this] {
        const char* env = std::getenv("BB_GPU_WATCHDOG_S");
        const int limit_s = env && env[0] ? std::atoi(env) : 10;
        if (limit_s > 0) {
            std::thread(Watchdog, &instance, limit_s).detach();
            std::printf("GPU: stall report on (%d s)\n", limit_s);
        }
        return true;
    }();
    (void)watchdog;
}

Semaphore::~Semaphore() {
    std::scoped_lock lock{registry_mutex};
    std::erase(registry, this);
}

void Semaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void Semaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    // bbport: a lost device ends the wait (2026-11-02: the GPU hung, Dozen returned device lost
    // and this loop retried until the game's own watchdog quit a minute later, with no log).
    vk::Result result;
    while ((result = instance.GetDevice().waitSemaphores(&wait_info, kWaitTimeout)) !=
           vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            instance.ReportDeviceLost("waiting for the GPU");
            BbSubmitRingDump(KnownGpuTick(), tick); // BB_SUBMIT_RING (vk_rasterizer.cpp)
            UNREACHABLE_MSG("Device lost while waiting for the GPU");
        }
    }
    Refresh();
}

} // namespace Vulkan

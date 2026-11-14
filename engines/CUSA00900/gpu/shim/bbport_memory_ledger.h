// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string_view>
#include "video_core/renderer_vulkan/vk_common.h"
#include <vk_mem_alloc.h>

// Resource suballocations are contained in VMA blocks. Never add both totals.
namespace BbMemoryLedger {
bool Enabled();
class Scope {
    char previous[192]{};
    bool enabled{};
public:
    explicit Scope(std::string_view name);
    ~Scope();
};
const VmaDeviceMemoryCallbacks* Callbacks();
void Resource(const char* action, const char* kind, VmaAllocator allocator,
              VmaAllocation allocation, std::uint64_t requested = 0,
              std::uint64_t usage = 0);
void Defer(VmaAllocator allocator, VmaAllocation allocation, std::uint64_t tick);
void Summary(VmaAllocator allocator, std::uint64_t staging);
}

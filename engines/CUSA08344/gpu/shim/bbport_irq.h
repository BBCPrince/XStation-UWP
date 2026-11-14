// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace BbIrq {
/// bbport (unless BB_ASYNC_IRQ=0): whether compute queue fence interrupts are delivered by a helper
/// thread instead of the GPU command thread.
bool Enabled();

/// Queues interrupt `irq` (a Platform::InterruptId) for the helper thread, which signals them in
/// order. Only the GPU command thread posts (single producer).
void Post(std::uint32_t irq);
} // namespace BbIrq

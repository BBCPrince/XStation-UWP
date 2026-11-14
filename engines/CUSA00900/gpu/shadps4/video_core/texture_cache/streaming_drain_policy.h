// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"

namespace VideoCore {

// A pressure check can find no CPU-backed images to retire. Repeating a full
// GPU drain in that unchanged live set only serializes rendering. Completed
// releases still run normally; fresh retirements, falling headroom and an
// emergency bypass this short retry interval.
class StreamingDrainPolicy {
public:
    bool ShouldDrain(u64 retired_bytes, u64 available_bytes, u64 now_ms) const {
        constexpr u64 MiB = 1024 * 1024;
        return retired_bytes != 0 || available_bytes < 64 * MiB || now_ms >= retry_at_ms ||
               (available_at_drain > available_bytes &&
                available_at_drain - available_bytes >= 8 * MiB);
    }

    void DidDrain(u64 available_bytes, u64 now_ms) {
        available_at_drain = available_bytes;
        retry_at_ms = now_ms + 1000;
    }

private:
    u64 available_at_drain{};
    u64 retry_at_ms{};
};

} // namespace VideoCore

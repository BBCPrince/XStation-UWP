// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// A submission owns its top-level command bytes until its decoder coroutine
// retires. SubmitDone/new frames cannot reuse a previous submission's storage.
class BbCommandBufferCopy {
public:
    BbCommandBufferCopy(std::span<const std::uint32_t> dcb,
                        std::span<const std::uint32_t> ccb)
        : draw(dcb.begin(), dcb.end()), constant(ccb.begin(), ccb.end()) {}

    std::span<const std::uint32_t> Draw() const { return draw; }
    std::span<const std::uint32_t> Constant() const { return constant; }

    // Capture a nested buffer only when its decoder starts, after earlier
    // waits/writes have executed. Keep that snapshot across coroutine yields.
    static void EnsureOwned(std::span<const std::uint32_t>& dcb,
                            std::span<const std::uint32_t>& ccb,
                            std::shared_ptr<const BbCommandBufferCopy>& owner) {
        if (!owner) owner = std::make_shared<BbCommandBufferCopy>(dcb, ccb);
        dcb = owner->Draw();
        ccb = owner->Constant();
    }

private:
    const std::vector<std::uint32_t> draw, constant;
};

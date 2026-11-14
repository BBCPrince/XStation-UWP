// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: D3D12's reason and DRED data after a device loss under Dozen.
#pragma once

namespace BbD3d12Lost {
/// Before the Vulkan instance (Dozen makes its D3D12 device while listing GPUs).
void EnableDred();
/// After a device loss: the removal reason, page fault and unfinished GPU commands, to stdout.
void Report();
/// A GPU hang D3D12 never resets (a queue waiting forever): removes the device so DRED's
/// breadcrumbs can be read. False when D3D12 cannot.
bool ForceRemoval();
} // namespace BbD3d12Lost

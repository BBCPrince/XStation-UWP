// XStation Wi-Fi transfer. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
struct TransferState {
    uint64_t received{};
    int active{}, finished{};
    std::wstring name;
    // Wi-Fi PKGs, converted while they arrive (2026-11-12)
    bool streaming{};                          // a PKG is arriving now
    int streamPhase{};                         // PkgStreamStatus::phase
    uint64_t streamStored{}, convertDone{}, convertTotal{};
    std::wstring streamTitle;
    int conversions{};                         // finished conversions so far (the dashboard prepares each new one)
    std::wstring convertedId, convertedTarget, lastPkgMessage;
    bool convertedUpdate{};
};
bool StartTransferServer(const std::wstring& dataRoot, std::string& pairingCode, int port);
TransferState GetTransferState();

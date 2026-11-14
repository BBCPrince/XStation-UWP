// XStation USB file access. SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>

// On the Xbox an app's plain file calls are denied on a USB drive (error 5); the *FromApp calls work for every file.
// This patches the import tables of the XStation exe, the C/C++ runtimes and every DLL in the package (the game
// engines and their libraries): a file call that is denied is tried again through its FromApp version. Calls that
// work are unchanged. Safe to call again after more DLLs are loaded; returns how many import slots it patched.
unsigned XStationUsbFilesInstall();
// How many denied calls the FromApp versions opened (for the logs).
uint64_t XStationUsbFilesFallbacks();
// Which modules the last install patched (for the logs).
std::wstring XStationUsbFilesReport();

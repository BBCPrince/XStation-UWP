// XStation: makes a game's prepared image (GameBoot) on the console, as the PC import tool's per-engine scripts do.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// True when XStation knows how to prepare this title on the console.
bool GamePrepareSupported(const std::wstring& titleId);

// Prepares <gameFolder>\GameData into <gameFolder>\GameBoot (boot-linked.bin, content.bin) and checks boot-linked.bin's
// SHA-256 against the one image the title's engine accepts. Nothing is written when it doesn't match (a different
// game version). `detail` gets a short log.
// `engineDir`: the title's engine folder (some recipes read their site list there).
bool GamePrepare(const std::wstring& titleId, const std::wstring& gameFolder, const std::wstring& engineDir, std::wstring& error, std::string& detail);

// The steps on their own, for the PC test: the image bytes for a game data folder.
bool GamePrepareImages(const std::wstring& titleId, const std::wstring& gameData, const std::wstring& engineDir, std::vector<uint8_t>& bootLinked,
                       std::vector<uint8_t>& content, std::wstring& error, std::string& detail, std::vector<uint8_t>* bootMemory = nullptr);

// The game data folder's name for this title (Bloodborne's engine uses its 1.09 names: GameData109, GameBoot109).
std::wstring GamePrepareDataFolder(const std::wstring& titleId);

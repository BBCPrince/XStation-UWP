// XStation (2026-11-14): The Last of Us's Xbox assets, made on the console from the user's own game files after a
// convert, so a fresh convert ends up as the tested setup (nothing from the game is shipped with XStation).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// Reduce the home texture archive: 846 environment textures lose one mip, 402 larger textures lose two,
// and 231 normal NPC textures lose one (about 488 MiB less). Reads `source` only when it is the
// 1.00 original (SHA-256 260933a8...), writes
// `destination` and keeps it only when it is the known result (c1cb0e4e...). 1 = written, 0 = nothing to do
// (already reduced, or another version), -1 = failed (destination removed).
int TlouReduceHomeTextures(const std::wstring& source, const std::wstring& destination, std::wstring& error, std::string& detail);

// One shader pack per game archive plus areas.txt (tools/psarc_make_packs.py and psarc_area_index.py) from
// <GameData>\build\ps4\main into `outFolder`. False on failure (no areas.txt written then).
bool TlouMakeShaderPacks(const std::wstring& mainFolder, const std::wstring& outFolder, std::wstring& error, std::string& detail);

// Both, for a prepared The Last of Us folder: the textures in place, the packs into <gameFolder>\shader-packs when
// that folder has no areas.txt yet. Failures only go to `detail` (the game still runs without them).
void TlouPrepareAssets(const std::wstring& gameFolder, std::string& detail);

// True when <gameFolder> (a prepared The Last of Us) still has the full home textures or no shader packs.
bool TlouAssetsNeeded(const std::wstring& gameFolder);

// Uncharted (CUSA02343, 2026-11-14): the 25 *-tstrm*.pak texture dictionaries lose the finest mip of the textures the
// runtime's mip-skip leaves out (tools/uc_trim_textures.py; 22.25 -> 5.81 GiB). A file is changed only when it has its
// original size, and kept only when the result has the tested SHA-256. Writes <gameFolder>\xstation-uncharted-assets.txt
// ("trimmed" when all 25 are, else "tried"). Returns the number trimmed now.
int UnchartedTrimTextures(const std::wstring& gameFolder, std::string& detail);
// True when a prepared Uncharted still has original-size dictionaries and wasn't tried yet (marks a hand-trimmed one).
bool UnchartedAssetsNeeded(const std::wstring& gameFolder);
// True when the marker says all 25 are trimmed (then the runtime's mip-skip must be off).
bool UnchartedTrimmed(const std::wstring& gameFolder);

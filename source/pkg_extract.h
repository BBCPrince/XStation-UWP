// XStation PKG extraction (a C++ port of OrbisPkgTool's PkgReader.ExtractAll, the reader the PC import tool uses).
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct PkgInfo {
    std::string contentId, titleId; // content ID from the header; title ID = its characters 7..15
    uint32_t contentType = 0;       // 0x1A game, 0x1E patch, 0x1B / 0x1C add-on
    uint64_t pkgSize = 0;
};

struct PkgProgress {
    std::atomic<uint64_t> done{0}, total{0}; // bytes written / to write (all PKGs of one conversion)
    std::atomic<uint32_t> files{0}, filesTotal{0};
};

// From the PKG's own param.sfo (Sc0, decrypted when needed): CATEGORY "gd" = game, "gp" = patch (an update's header
// can say "game"), APP_VER, VERSION, TITLE (UTF-8), TITLE_ID.
struct PkgParams {
    std::string category, appVer, version, title, titleId;
    std::vector<uint8_t> icon; // icon0.png, when the PKG has one
};
bool PkgReadParams(const std::wstring& pkg, PkgParams& params, std::wstring& error);

// 2026-11-18: the one message every
// convert / copy / Wi-Fi path shows when the game doesn't fit:
// "Not enough storage. This game needs 59.6 GB of free space on the Xbox, and it has 20.3 GB free.
//  Free up at least 39.3 GB (delete games or files you don't need) and try again."
// `need` and `freeBytes` in bytes; `where` names the drive ("the Xbox", "the USB drive"); `lead` replaces the first
// sentence when the case needs it.
std::wstring NotEnoughStorageText(uint64_t need, uint64_t freeBytes, const std::wstring& where = L"the Xbox",
                                  const std::wstring& lead = L"Not enough storage.");

// Reads the header (no decryption). False with `error` set when it isn't a PS4 PKG.
bool PkgReadInfo(const std::wstring& pkg, PkgInfo& info, std::wstring& error);

// Extracts PKGs into one game folder the way the PC import tool merges them: the Image0 files of every PKG in order
// (later ones win), then the Sc0 files of every PKG in order into sce_sys\ (so the last PKG's param.sfo wins).
// Fake PKGs (passcode of zeros) and official ones (RSA-recovered key 3) are read; a custom-key PKG fails.
// `progress` counts bytes (sizes are summed up front); `cancel` stops between blocks. Files already present with the
// right size are kept when `resume` is set.
// `freeBytes` (0 = unknown): the target drive's free space; the files still to write plus 512 MiB must fit, or nothing
// is written.
//
// Using the PKGs up (USB, 2026-11-12), for a drive without room for the PKG and the game together: the files are written
// from the end of each PKG backwards, the PKG is cut short (truncated) once no file left needs its end, and the PKGs are
// deleted when the game is done. Only the difference between the game and the PKGs (plus a margin) has to be free.
// It runs only when the normal way doesn't fit and `shrink->allowed` is set (the player said yes), or when an earlier
// conversion already cut these PKGs (<outDir's parent>\.xstation-shrink, then it is resumed the same way).
struct PkgShrink {
    bool allowed = false;     // in: the player agreed to use the PKGs up
    bool used = false;        // out: this conversion used them up (they are deleted now)
    uint64_t need = 0;        // out: free space using the PKGs up needs (set when the normal way doesn't fit)
    uint64_t normalNeed = 0;  // out: free space the normal way needs (same)
};
// `rewriteUpdates` (2026-11-14): an update added to a game already converted into outDir (pkgs = game, update): every
// file the update brings is written again even when one of the same size is there (the game's own version), the
// game's files are kept. Not for a resumed using-up conversion (its PKGs are already cut).
bool PkgExtractGame(const std::vector<std::wstring>& pkgs, const std::wstring& outDir, PkgProgress* progress,
                    const std::atomic<bool>* cancel, bool resume, std::wstring& error, uint64_t freeBytes = 0,
                    PkgShrink* shrink = nullptr, bool rewriteUpdates = false);

// Wi-Fi streaming check (PC tool): how much of the PKG must arrive before the files can be planned, and the peak space a
// convert while receiving would need with the PKG kept as `chunk`-byte pieces.
bool PkgLayoutReport(const std::wstring& pkg, uint64_t chunk, std::string& report);

// Wi-Fi (2026-11-12): a PKG converted while it arrives, so the drive never holds the PKG and the game together.
// The receiver calls Append with the bytes in order and Finish at the end. A worker thread reads the first pieces
// (header, keys, file tables), plans the game into <gamesRoot>\<title ID>\<dataFolder(id)>, writes each file once its
// bytes are in and deletes every 256 MiB piece of the PKG that no file left needs. A game PKG converts into a new folder
// (an unfinished one is resumed); an update converts on top of its game. Anything else (retail or custom-key PKGs,
// add-ons, an update whose game isn't there) is kept whole at `keepPath`, as before.
struct PkgStreamStatus {
    std::atomic<int> phase{0};       // 0 reading the start, 1 converting, 2 keeping the PKG, 3 converted, 4 failed
    std::atomic<uint64_t> stored{0}; // PKG bytes on the drive right now
    PkgProgress progress;            // game bytes written / to write
    std::mutex lock;                 // for the strings below
    std::wstring error, titleId, title, target; // target: the game folder
    bool update = false;
};
class PkgStream {
public:
    PkgStream(const std::wstring& spoolDir, const std::wstring& keepPath, const std::wstring& gamesRoot, uint64_t size,
              std::function<std::wstring(const std::wstring&)> dataFolder, std::function<uint64_t()> freeBytes, PkgStreamStatus& status);
    ~PkgStream();
    // False: stop receiving (the reason is in status.error).
    bool Append(const uint8_t* data, size_t n);
    // The upload ended. True when the game was converted or the PKG was kept whole.
    bool Finish(bool complete);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

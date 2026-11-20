// XStation dashboard. SPDX-License-Identifier: GPL-2.0-or-later
// Runs with no game runtime loaded. Play writes LocalState\play.txt and restarts the app, so the
// game gets a clean process and the whole memory budget (game_app.cpp reads the file at startup).
#include <windows.h>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Text.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include "dashboard.h"
#include "transfer.h"
#include "usb_files.h"
#include "pkg_extract.h"
#include "game_prepare.h"
#include "tlou_assets.h"
#include <fileapifromapp.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Security.ExchangeActiveSyncProvisioning.h>
#include <fileapifromapp.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Storage;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Media::Imaging;
using namespace Windows::UI::Xaml::Shapes;
namespace Anim = Windows::UI::Xaml::Media::Animation;

namespace {

constexpr double kWidth = 1280, kHeight = 720, kMarginX = 44;
constexpr double kTile = 76, kTileSelected = 92, kTileGap = 10;

SolidColorBrush Solid(uint8_t a, uint8_t r, uint8_t g, uint8_t b) { return SolidColorBrush(Color{a, r, g, b}); }
SolidColorBrush White(uint8_t a = 255) { return Solid(a, 255, 255, 255); }

struct Tested {
    const wchar_t* id; const wchar_t* speed; const wchar_t* status;
};
// From the project notes (measured on the Xbox Series X). Anything else reads "Not tested".
const Tested kTested[] = {
    {L"CUSA02343", L"About 59 FPS", L"Playable"},
    {L"CUSA00900", L"About 59 FPS", L"Reduced quality"},
    {L"CUSA00552", L"About 30 FPS", L"Experimental"},
    {L"CUSA07408", L"About 15 FPS", L"Experimental"},
    {L"CUSA08344", L"About 25 FPS", L"Playable"}, // 2026-11-18: intro ~28, chapter 1 22-25 FPS, looks right
    {L"CUSA02299", L"No gameplay yet", L"Crashes early"},
};

struct Compatible { const wchar_t* id; const wchar_t* name; const wchar_t* version; const wchar_t* region; };
const Compatible kCompatible[] = {
    {L"CUSA02343", L"Uncharted: The Nathan Drake Collection", L"1.00, no update", L"Europe"},
    {L"CUSA00900", L"Bloodborne", L"1.09 (game + update 1.09)", L"US"},
    {L"CUSA00552", L"The Last of Us Remastered", L"1.00, no update", L"US"},
    {L"CUSA08344", L"Detroit: Become Human", L"1.00, no update", L"US"},
    {L"CUSA07408", L"God of War", L"1.35 (game + update 1.35)", L"US"},
};

const wchar_t* kSetAsGame = L"XStation is set as an App, so the Xbox gives it only 1 GB of memory and games can't start. "
    L"Set it to Game: in Dev Home, highlight XStation, press the View button, choose App details, change the type to Game, "
    L"then start XStation again.";
bool RunsAsApp() {
    return Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox" &&
           Windows::System::MemoryManager::AppMemoryUsageLimit() < (2ull << 30);
}

bool HasFpsChoice(const std::wstring& title) { const auto id = GameEngineId(title); return id != L"CUSA07408" && id != L"CUSA02299"; }
// Games whose 60 is FSR frame generation (the engine has their camera hookup), 30 by default (2026-11-09).
bool HasFsrChoice(const std::wstring& title) { return GameEngineId(title) == L"CUSA00552"; }
// Games with FSR 3.1 upscaling (their engine has the camera hookup and the upscale trigger): Bloodborne (2026-11-09).
bool HasFsrUpscale(const std::wstring& title) { return GameEngineId(title) == L"CUSA00900"; }
// 2026-11-17 (other regions): the engine folder of a title (its own, or the one its image matched).
bool EngineExists(const std::wstring& package, const std::wstring& title) {
    const std::wstring dir = package + L"\\Engines\\" + GameEngineId(title);
    const DWORD a1 = GetFileAttributesW((dir + L"\\tlou-uwp-runtime.dll").c_str()), a2 = GetFileAttributesW((dir + L"\\bb-uwp-runtime.dll").c_str());
    return a1 != INVALID_FILE_ATTRIBUTES || a2 != INVALID_FILE_ATTRIBUTES;
}

struct Game {
    std::wstring id, title, version, folder, uriBase, icon, hero;
    bool hasBoot{}, hasEngine{}, removable{};
    bool mounted{}, usbMissing{}, copying{}; // mounted: plays from a USB folder; copying: a copy to the Xbox is unfinished
    uint64_t bytes{}, seconds{}, lastPlayed{};
    int fps = 60; // what the game page shows; Titles\<id>\frame-limit.txt holds 30, or 0 for 60 (the host reads it)
    bool fsr{}; // HasFsrChoice games: 60 = FSR frame generation (Titles\<id>\frame-gen.txt holds 1)
    bool upscale = false; // HasFsrUpscale games: off = plain 540p (default); on (fsr-upscale.txt holds 1) = 540p + FSR to 1080p
    bool cpu = false; // the engine's FPS and CPU box: cpu-counter.txt holds 1 or 0; no file = off (every game, 2026-11-15)
};

struct UsbItem {
    std::wstring path, name, id, title; // path: the file or game folder on the drive (plain file calls reach it through usb_files.cpp)
    uint64_t bytes{};
    bool pkg{}, prepared{};
    std::string category; // PKGs: "gd" game, "gp" update (param.sfo CATEGORY)
    std::wstring version;
    BitmapImage art{nullptr}; // icon0.png of the folder or the PKG
    // 2026-11-18: a game PKG whose engine plays it with an update, and that update's PKG on the same drive: one row,
    // A converts both (the update's own row is left out of the list).
    std::wstring updatePath, updateVersion;
    uint64_t updateBytes{};
    bool paired{};
    // 2026-11-18: the compatibility check from the PKG's own game files, before any convert: -2 not checked yet, -1 it
    // couldn't be checked (compatNote says why), 0 not compatible, 1 compatible (compatNote: the engine's game).
    int compat = -2;
    std::wstring compatNote;
};
std::wstring CompatKey(const UsbItem& u) { return u.path + L"|" + std::to_wstring(u.bytes) + L"|" + u.updatePath + L"|" + std::to_wstring(u.updateBytes); }
// "01.09" -> "1.09" for the player.
std::wstring VersionText(std::wstring v) {
    while (v.size() > 1 && v[0] == L'0' && v[1] != L'.') v.erase(0, 1);
    return v;
}

struct Mount {
    std::wstring id, folder, title;
};

std::wstring FromUtf8(const std::string& text);
std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n, nullptr, nullptr);
    return out;
}

struct PkgFile {
    std::wstring name, titleId;
    uint64_t bytes{};
};

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// XStation (2026-11-15): save export / import. The FromApp calls work on the USB drive and in LocalState alike.
bool IsFolderFromApp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}
// One file, read and written through CreateFileFromAppW (no SECURITY_ATTRIBUTES: the broker refuses inheritable handles).
// CopyFileFromAppW failed for every save on the Xbox's USB drive (2026-11-15). On failure `error` names the step and code.
bool CopyFileThroughApp(const std::wstring& from, const std::wstring& to, uint64_t& bytes, std::wstring& error) {
    HANDLE in = CreateFileFromAppW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) { error = L"open " + from + L" (error " + std::to_wstring(GetLastError()) + L")"; return false; }
    HANDLE out = CreateFileFromAppW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        error = L"create " + to + L" (error " + std::to_wstring(GetLastError()) + L")";
        CloseHandle(in);
        return false;
    }
    std::vector<char> buffer(1 << 20);
    bool ok = true;
    for (;;) {
        DWORD got = 0, put = 0;
        if (!ReadFile(in, buffer.data(), DWORD(buffer.size()), &got, nullptr)) { error = L"read " + from + L" (error " + std::to_wstring(GetLastError()) + L")"; ok = false; break; }
        if (!got) break;
        if (!WriteFile(out, buffer.data(), got, &put, nullptr) || put != got) { error = L"write " + to + L" (error " + std::to_wstring(GetLastError()) + L")"; ok = false; break; }
        bytes += got;
    }
    if (ok && !FlushFileBuffers(out)) { error = L"flush " + to + L" (error " + std::to_wstring(GetLastError()) + L")"; ok = false; }
    CloseHandle(out);
    CloseHandle(in);
    if (!ok) DeleteFileFromAppW(to.c_str());
    return ok;
}
// Copies every file under `from` into `to` (made as needed). False on the first failure, with `error` set.
bool CopyTreeFromApp(const std::wstring& from, const std::wstring& to, uint64_t& files, uint64_t& bytes, std::wstring& error) {
    if (!CreateDirectoryFromAppW(to.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"make folder " + to + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExFromAppW((from + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        error = L"list " + from + L" (error " + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    bool ok = true;
    do {
        const std::wstring name = f.cFileName;
        if (name == L"." || name == L"..") continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = CopyTreeFromApp(from + L"\\" + name, to + L"\\" + name, files, bytes, error);
        } else {
            ok = CopyFileThroughApp(from + L"\\" + name, to + L"\\" + name, bytes, error);
            if (ok) ++files;
        }
    } while (ok && FindNextFileW(h, &f));
    FindClose(h);
    return ok;
}
// Removes a folder tree this code made (a failed import copy).
void DeleteTreeFromApp(const std::wstring& path) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExFromAppW((path + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = f.cFileName;
            if (name == L"." || name == L"..") continue;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTreeFromApp(path + L"\\" + name);
            else DeleteFileFromAppW((path + L"\\" + name).c_str());
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryFromAppW(path.c_str());
}
int CheckPkgsCompatible(const std::vector<std::wstring>& pkgs, const std::wstring& titleId, const std::wstring& scratch,
                        const std::wstring& enginesRoot, std::wstring& engine, std::wstring& error, std::string& detail) {
    DeleteTreeFromApp(scratch);
    CreateDirectoryFromAppW(scratch.c_str(), nullptr);
    static const std::vector<std::string> only{"eboot.bin", "sce_module/", "sce_sys/param.sfo"};
    int result = -1;
    if (PkgExtractGame(pkgs, scratch, nullptr, nullptr, false, error, 0, nullptr, false, &only))
        result = GameMatchData(titleId, scratch, enginesRoot, engine, detail) ? 1 : 0;
    DeleteTreeFromApp(scratch);
    return result;
}
// A game's save folder: Bloodborne's engine keeps saves in game-user, the others in tlou-bb194-user.
std::wstring SaveFolder(const std::wstring& local, const std::wstring& id) {
    const std::wstring title = local + L"\\Titles\\" + id;
    for (const wchar_t* user : {L"game-user", L"tlou-bb194-user"})
        if (IsFolderFromApp(title + L"\\" + user + L"\\savedata")) return title + L"\\" + user + L"\\savedata";
    return title + (GameEngineId(id) == L"CUSA00900" ? L"\\game-user\\savedata" : L"\\tlou-bb194-user\\savedata");
}
std::wstring SaveStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t s[40];
    swprintf(s, 40, L"%04u-%02u-%02u %02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
    return out;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), n);
    return out;
}

// param.sfo: strings by key. Every offset is checked against the file size.
std::map<std::string, std::string> ReadSfo(const std::wstring& path) {
    std::map<std::string, std::string> out;
    std::ifstream file(path, std::ios::binary);
    if (!file) return out;
    std::string data((std::istreambuf_iterator<char>(file)), {});
    auto u16 = [&](size_t at) { return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if (data.size() < 20 || memcmp(data.data(), "\0PSF", 4)) return out;
    const size_t keys = u32(8), values = u32(12), count = u32(16);
    if (count > 1024 || 20 + count * 16 > data.size()) return out;
    for (size_t i = 0; i < count; ++i) {
        const size_t e = 20 + i * 16;
        const uint32_t format = u16(e + 2), length = u32(e + 4);
        const size_t key = keys + u16(e), value = values + u32(e + 12);
        if (format != 0x0204 || key >= data.size() || value + length > data.size()) continue;
        const char* k = data.data() + key;
        std::string name(k, strnlen(k, data.size() - key));
        std::string text(data.data() + value, length);
        text.resize(strnlen(text.c_str(), text.size()));
        out[name] = text;
    }
    return out;
}

std::vector<std::wstring> Folders(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(f.cFileName, L".") && wcscmp(f.cFileName, L".."))
            out.push_back(f.cFileName);
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return out;
}

void ReadStats(const std::wstring& path, Game& game) {
    std::ifstream f(path);
    if (f) f >> game.seconds >> game.lastPlayed;
}

void ScanGames(const std::wstring& root, const std::wstring& package, const std::wstring& local, std::vector<Game>& games) {
    for (const auto& id : Folders(root)) {
        Game g;
        g.id = id;
        g.folder = root + L"\\" + id;
        // Bloodborne's engine uses its 1.09 folder names.
        const bool update109 = Exists(g.folder + L"\\GameData109\\sce_sys\\param.sfo");
        const std::wstring sys = g.folder + (update109 ? L"\\GameData109\\sce_sys\\" : L"\\GameData\\sce_sys\\");
        if (!Exists(sys + L"param.sfo")) continue;
        auto sfo = ReadSfo(sys + L"param.sfo");
        g.title = sfo.count("TITLE") ? Widen(sfo["TITLE"]) : id;
        g.version = sfo.count("APP_VER") ? Widen(sfo["APP_VER"]) : (sfo.count("VERSION") ? Widen(sfo["VERSION"]) : L"");
        g.uriBase = sys;
        if (g.version.size() > 1 && g.version[0] == L'0') g.version.erase(0, 1);
        if (Exists(sys + L"icon0.png")) g.icon = L"icon0.png";
        for (const wchar_t* name : {L"pic1.png", L"pic0.png", L"icon0.png"}) {
            if (Exists(sys + name)) { g.hero = name; break; }
        }
        g.hasBoot = Exists(g.folder + (update109 ? L"\\GameBoot109\\boot-linked.bin" : L"\\GameBoot\\boot-linked.bin"));
        g.copying = Exists(g.folder + L"\\.xstation-copying");
        g.hasEngine = EngineExists(package, id);
        std::ifstream info(g.folder + L"\\xstation-game.txt");
        if (info) info >> g.bytes;
        ReadStats(local + L"\\Titles\\" + id + L"\\stats.txt", g);
        {
            std::ifstream limit(local + L"\\Titles\\" + id + L"\\frame-limit.txt");
            int value = 0;
            if (limit >> value && value == 30) g.fps = 30;
        }
        if (HasFsrUpscale(id)) {
            std::ifstream upscale(local + L"\\Titles\\" + id + L"\\fsr-upscale.txt");
            int value = 0;
            if (upscale >> value) g.upscale = value != 0;
        }
        {
            std::ifstream counter(local + L"\\Titles\\" + id + L"\\cpu-counter.txt");
            int value = 0;
            g.cpu = counter >> value ? value != 0 : false; // 2026-11-15 (user): off for every game unless turned on
        }
        if (HasFsrChoice(id)) {
            std::ifstream frameGen(local + L"\\Titles\\" + id + L"\\frame-gen.txt");
            int value = 0;
            g.fsr = frameGen >> value && value == 1;
            g.fps = g.fsr ? 60 : 30;
        }
        games.push_back(std::move(g));
    }
}

void DeleteTree(const std::wstring& dir, std::atomic<uint64_t>& files, std::atomic<uint64_t>& failures) {
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            const std::wstring path = dir + L"\\" + f.cFileName;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    if (!RemoveDirectoryW(path.c_str())) ++failures;
                } else DeleteTree(path, files, failures);
            } else {
                if (f.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (DeleteFileW(path.c_str())) ++files; else ++failures;
            }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    if (!RemoveDirectoryW(dir.c_str())) ++failures;
}

std::vector<PkgFile> ScanImport(const std::wstring& dir) {
    std::vector<PkgFile> out;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW((dir + L"\\*.pkg").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        PkgFile p;
        p.name = f.cFileName;
        p.bytes = (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
        std::ifstream file(dir + L"\\" + f.cFileName, std::ios::binary);
        char head[0x70]{};
        // PKG header: magic "\x7FCNT", content ID at 0x40 (XX0000-CUSA00000_00-...).
        if (file.read(head, sizeof(head)) && !memcmp(head, "\x7F" "CNT", 4)) {
            std::string content(head + 0x40, strnlen(head + 0x40, 36));
            if (content.size() > 16) p.titleId = Widen(content.substr(7, 9));
        }
        out.push_back(std::move(p));
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return out;
}

std::wstring FormatHours(uint64_t seconds) {
    if (!seconds) return L"0 h";
    wchar_t text[32];
    const double hours = seconds / 3600.0;
    if (hours < 1.0) swprintf(text, 32, L"%llu min", (unsigned long long)(seconds / 60 ? seconds / 60 : 1));
    else swprintf(text, 32, L"%.1f h", hours);
    return text;
}

// "D: 39.1 GB free of 136 GB" for the drive a path is on.
std::wstring DriveLine(const std::wstring& path) {
    ULARGE_INTEGER freeBytes{}, total{};
    if (path.size() < 2 || !GetDiskFreeSpaceExW(path.c_str(), &freeBytes, &total, nullptr)) return path.substr(0, 2) + L" unknown";
    wchar_t text[64];
    swprintf(text, 64, L"%c: %.1f GB free of %.0f GB", towupper(path[0]), freeBytes.QuadPart / 1073741824.0, total.QuadPart / 1073741824.0);
    return text;
}

std::wstring FormatAgo(uint64_t unixTime) {
    if (!unixTime) return L"Not played yet";
    const int64_t now = _time64(nullptr), days = (now - (int64_t)unixTime) / 86400;
    if (days <= 0) return L"Played today";
    if (days == 1) return L"Played yesterday";
    return L"Played " + std::to_wstring(days) + L" days ago";
}

std::wstring FormatBytes(uint64_t bytes) {
    if (!bytes) return L"—";
    wchar_t text[32];
    swprintf(text, 32, bytes >= (1ull << 30) ? L"%.1f GB" : L"%.0f MB", bytes >= (1ull << 30) ? bytes / 1073741824.0 : bytes / 1048576.0);
    return text;
}

struct DashApp : ApplicationT<DashApp> {
    std::wstring local, package, dataRoot;
    bool confirmOpen = false, gamesDriveMissing = false;
    int confirmKind = 0; // what the open question is about: 0 deleting the game, 1 turning FSR on, 2 deleting converted PKGs,
                         // 3 using a PKG up to convert it (no room for the PKG and the game together), 4 importing saves,
                         // 5 a newer XStation on GitHub (A opens its page)
    // XStation 2026-11-17: the newest release of the public repo, when it is newer than this package (CheckForUpdate)
    std::wstring updateTag, updateMine, updateNotes, updateUrl;
    std::atomic<bool> updateReady{false};
    bool updateTestDev = false;
    bool savesBusy = false;               // a save export or import is running
    std::vector<std::wstring> importChoices; // exports of the selected game on the USB drive, newest first
    size_t importPick = 0;
    std::wstring importRoot;              // that USB drive
    TextBlock storage{nullptr};
    int storageWait = 0;
    std::atomic<bool> deleting{false}, deleteFinished{false};
    std::atomic<uint64_t> deletedFiles{0}, deleteFailures{0};
    std::wstring deletingTitle;
    bool devPlay = false, devDelete = false, devLogs = false;
    int devWait = 0;
    std::vector<UsbItem> usb;
    std::map<std::wstring, std::pair<int, std::wstring>> compatCache; // CompatKey -> (compat, compatNote); UI thread only
    std::atomic<bool> compatRunning{false};
    size_t usbSelected = 0;
    bool usbScanning = false, usbScannedOnce = false, usbProbeWanted = false;
    std::wstring devUsbAction; // dev switch: "copy <id|pkg>" or "mount <id>" after the first scan
    // USB copy (worker thread): 0 none, 1 counting, 2 copying, 3 done, 4 failed. Poll shows it.
    std::atomic<int> copyState{0};
    std::atomic<uint64_t> copyDone{0}, copyTotal{0}, copyFiles{0}, copyFilesTotal{0};
    std::atomic<bool> copyArtReady{false};
    // XStation (2026-11-11): mounted games' USB folders are read on a worker, never on the UI thread. After a
    // console restart the drive answered so slowly that this scan in Rescan() ran past the Xbox's start limit
    // ("XStation took too long to start", 0x8027025a) on every launch.
    std::mutex mountScanMutex;
    std::map<std::wstring, Game> mountFound;   // id -> the game found in its mounted folder
    std::map<std::wstring, bool> mountChecked; // id -> its folder was read (found or not)
    std::atomic<bool> mountScanBusy{false}, mountScanReady{false}, mountDrivesAsked{false}, mountMissing{false};
    ULONGLONG mountScanAt = 0;
    void StartMountScan(const std::vector<Mount>& mounts) {
        if (mounts.empty()) return;
        if (mountScanAt && GetTickCount64() - mountScanAt < 5000) return; // at most every 5 s
        if (mountScanBusy.exchange(true)) return;
        mountScanAt = GetTickCount64();
        std::thread([this, mounts, root = package, data = local] {
            // A new XStation process can't read the USB drive (the FromApp calls fail) until it has asked for the drive
            // list once: after a console restart a mounted game showed "USB missing" until the Add Game page was opened
            // (2026-11-12). Ask on the first scan, and again while a mounted game is missing.
            if (!mountDrivesAsked || mountMissing) {
                try { KnownFolders::RemovableDevices().GetFoldersAsync().get(); } catch (...) {}
                mountDrivesAsked = true;
            }
            std::map<std::wstring, Game> found;
            std::map<std::wstring, bool> checked;
            for (const auto& m : mounts) {
                std::vector<Game> list;
                const size_t slash = m.folder.find_last_of(L'\\');
                if (slash != std::wstring::npos) ScanGames(m.folder.substr(0, slash), root, data, list);
                for (auto& g : list) if (g.id == m.id) found[m.id] = g;
                checked[m.id] = true;
            }
            mountMissing = found.size() < mounts.size();
            {
                std::scoped_lock lock{mountScanMutex};
                mountFound = std::move(found);
                mountChecked = std::move(checked);
            }
            mountScanBusy = false;
            mountScanReady = true;
        }).detach();
    }
    std::mutex copyLock;
    std::wstring copyError, copyTitle, copyId, copyTarget; // under copyLock while the worker runs
    bool copyIsGame = false;
    int shownCopyState = 0, progressWait = 0;
    uint64_t speedBytes = 0;
    double copySpeed = 0;
    std::chrono::steady_clock::time_point speedAt{}, copyBegan{}, progressLoggedAt{};
    std::vector<Grid> tileBars;
    std::wstring usbStatus, wifiAddress;
    std::string pairing;
    bool wifiStarted = false;
    TextBlock wifiText{nullptr}, wifiProgress{nullptr}, usbText{nullptr};
    TextBlock usbCopyLine{nullptr}, copyLine{nullptr};
    // Add Game page (layout B): progress row, USB rows, "From your PC" card.
    Border progressRow{nullptr}, progressArt{nullptr};
    TextBlock progressTitle{nullptr}, progressRight{nullptr}, usbHeader{nullptr}, wifiAddressText{nullptr}, wifiCodeText{nullptr}, wifiStatusText{nullptr};
    std::wstring usbDriveLine;
    BitmapImage copyArt{nullptr};
    bool wifiWasActive = false, wifiRowShown = false;
    // Wi-Fi PKGs converted while they arrive (2026-11-12): each finished conversion is prepared here.
    int wifiConversionsSeen = 0;
    std::atomic<bool> wifiPrepared{false};
    std::wstring wifiPrepareMessage; // under copyLock
    Border usbBarTrack{nullptr}, copyBarTrack{nullptr};
    Grid usbBar{nullptr}, copyBar{nullptr};
    StackPanel copyRow{nullptr};
    StackPanel usbList{nullptr}, hintsGames{nullptr}, hintsAdd{nullptr};
    StackPanel compatPage{nullptr}, compatList{nullptr}, hintsCompat{nullptr}; // tab 2, Compatible (2026-11-18)
    TextBlock tabCompat{nullptr};
    std::vector<Game> games;
    size_t selected = 0;
    int tab = 0;
    bool detailsOpen = false, launching = false;

    Image hero{nullptr};
    StackPanel tileRow{nullptr};
    TranslateTransform tileShift{nullptr};
    TextBlock tileLabel{nullptr}, title{nullptr}, meta{nullptr}, clock{nullptr}, tabGames{nullptr}, tabAdd{nullptr}, notice{nullptr};
    TextBlock cardTime{nullptr}, cardTimeSub{nullptr}, cardSpeed{nullptr}, cardSpeedSub{nullptr}, cardSize{nullptr}, cardSizeSub{nullptr},
        cardStatus{nullptr}, cardStatusSub{nullptr}, playLabel{nullptr}, detailsText{nullptr}, fpsLabel{nullptr};
    Border playButton{nullptr}, details{nullptr}, fpsButton{nullptr}, upscaleButton{nullptr};
    // XStation (2026-11-15): the Details view (confirm questions still use detailsText).
    StackPanel detailsView{nullptr};
    TextBlock dTitle{nullptr}, dSub{nullptr}, dFolder{nullptr}, dSaves{nullptr}, dEngine{nullptr}, dBoot{nullptr}, dCpu{nullptr},
        dStatus{nullptr};
    Border dStatusBox{nullptr};
    FrameworkElement hintFps{nullptr}, hintUpscale{nullptr};
    TextBlock upscaleLabel{nullptr};
    Polygon playIcon{nullptr};
    Grid gamesPage{nullptr};
    StackPanel addPage{nullptr}, importList{nullptr};
    DispatcherTimer timer{nullptr};
    // XStation (2026-11-13): pop-up notification (why the last game closed); hides itself, or B closes it.
    Border toast{nullptr};
    TextBlock toastTitle{nullptr}, toastBody{nullptr};
    Media::TranslateTransform toastMove{nullptr};
    std::chrono::steady_clock::time_point toastUntil{};
    bool toastShown = false;
    uint32_t padPrevious = 0;
    bool padSeen = false;
    int repeatDirection = 0;
    std::chrono::steady_clock::time_point repeatAt{};

    TextBlock Text(const std::wstring& value, double size, SolidColorBrush brush = nullptr) {
        TextBlock t;
        t.Text(value);
        t.FontSize(size);
        t.Foreground(brush ? brush : White());
        t.TextTrimming(TextTrimming::CharacterEllipsis);
        return t;
    }

    Border Chip(const std::wstring& label) {
        Border b;
        b.Background(White(46));
        b.CornerRadius(CornerRadius{4, 4, 4, 4});
        b.Padding(Thickness{7, 2, 7, 2});
        b.VerticalAlignment(VerticalAlignment::Center);
        b.Child(Text(label, 11));
        return b;
    }

    // A progress bar: a track with a filled part; SetBar changes the split.
    Border Bar(Grid& inner, double width, double height) {
        Border track;
        track.Background(White(55));
        track.CornerRadius(CornerRadius{height / 2, height / 2, height / 2, height / 2});
        track.Height(height);
        if (width > 0) track.Width(width);
        inner = Grid();
        ColumnDefinition done, left;
        done.Width(GridLengthHelper::FromValueAndType(0, GridUnitType::Star));
        left.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        inner.ColumnDefinitions().Append(done);
        inner.ColumnDefinitions().Append(left);
        Border fill;
        fill.Background(Solid(255, 40, 130, 230));
        fill.CornerRadius(CornerRadius{height / 2, height / 2, height / 2, height / 2});
        Grid::SetColumn(fill, 0);
        inner.Children().Append(fill);
        track.Child(inner);
        return track;
    }
    static void SetBar(Grid const& inner, double fraction) {
        if (!inner) return;
        fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
        inner.ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromValueAndType(fraction, GridUnitType::Star));
        inner.ColumnDefinitions().GetAt(1).Width(GridLengthHelper::FromValueAndType(1 - fraction, GridUnitType::Star));
    }

    FrameworkElement Hint(const std::wstring& letter, const std::wstring& label, Color color) {
        StackPanel row;
        row.Orientation(Orientation::Horizontal);
        row.Spacing(6);
        row.VerticalAlignment(VerticalAlignment::Center);
        Grid dot;
        dot.Width(18); dot.Height(18);
        Ellipse circle;
        circle.Fill(SolidColorBrush(color));
        dot.Children().Append(circle);
        auto letterBlock = Text(letter, 10);
        letterBlock.HorizontalAlignment(HorizontalAlignment::Center);
        letterBlock.VerticalAlignment(VerticalAlignment::Center);
        letterBlock.FontWeight(Windows::UI::Text::FontWeights::Bold());
        dot.Children().Append(letterBlock);
        row.Children().Append(dot);
        auto text = Text(label, 11, White(215));
        text.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(text);
        return row;
    }

    // A controller button as a small rounded label (wider than Hint's dot, so "VIEW" and "☰" fit).
    Border ButtonChip(const std::wstring& label, Color color) {
        Border b;
        b.Background(SolidColorBrush(color));
        b.CornerRadius(CornerRadius{10, 10, 10, 10});
        b.MinWidth(20);
        b.Height(20);
        b.Padding(Thickness{6, 0, 6, 0});
        b.VerticalAlignment(VerticalAlignment::Center);
        auto t = Text(label, 10);
        t.FontWeight(Windows::UI::Text::FontWeights::Bold());
        t.HorizontalAlignment(HorizontalAlignment::Center);
        t.VerticalAlignment(VerticalAlignment::Center);
        b.Child(t);
        return b;
    }
    Border Divider() {
        Border line;
        line.Height(1);
        line.Background(White(34));
        return line;
    }
    // One "label  value" line of the info grid.
    TextBlock InfoRow(Grid& grid, const std::wstring& label) {
        const int row = grid.RowDefinitions().Size();
        grid.RowDefinitions().Append(RowDefinition());
        auto name = Text(label, 12, White(140));
        name.Margin(Thickness{0, 3, 16, 3});
        Grid::SetRow(name, row);
        grid.Children().Append(name);
        auto value = Text(L"", 13, White(230));
        value.TextWrapping(TextWrapping::Wrap);
        value.TextTrimming(TextTrimming::None);
        value.Margin(Thickness{0, 3, 0, 3});
        Grid::SetRow(value, row);
        Grid::SetColumn(value, 1);
        grid.Children().Append(value);
        return value;
    }
    // One option: button chip, what it does, and (for the CPU counter) its state on the right.
    void OptionRow(StackPanel& list, Border chip, const std::wstring& label, TextBlock* state) {
        Grid row;
        ColumnDefinition a, b, s;
        a.Width(GridLengthHelper::FromValueAndType(64, GridUnitType::Pixel));
        b.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        s.Width(GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(a);
        row.ColumnDefinitions().Append(b);
        row.ColumnDefinitions().Append(s);
        row.Padding(Thickness{12, 9, 14, 9});
        row.Background(White(14));
        row.CornerRadius(CornerRadius{6, 6, 6, 6});
        chip.HorizontalAlignment(HorizontalAlignment::Left);
        row.Children().Append(chip);
        auto text = Text(label, 13, White(235));
        text.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(text, 1);
        row.Children().Append(text);
        if (state) {
            *state = Text(L"", 13);
            state->FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            state->VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(*state, 2);
            row.Children().Append(*state);
        }
        list.Children().Append(row);
    }
    StackPanel BuildDetailsView() {
        detailsView = StackPanel();
        detailsView.Spacing(14);
        detailsView.Visibility(Visibility::Collapsed);
        dTitle = Text(L"", 24);
        dTitle.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        dSub = Text(L"", 13, White(160));
        StackPanel head;
        head.Spacing(2);
        head.Children().Append(dTitle);
        head.Children().Append(dSub);
        detailsView.Children().Append(head);
        detailsView.Children().Append(Divider());
        Grid info;
        ColumnDefinition l, v;
        l.Width(GridLengthHelper::Auto());
        v.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        info.ColumnDefinitions().Append(l);
        info.ColumnDefinitions().Append(v);
        dFolder = InfoRow(info, L"Game folder");
        dSaves = InfoRow(info, L"Saves");
        dEngine = InfoRow(info, L"Engine");
        dBoot = InfoRow(info, L"Prepared image");
        detailsView.Children().Append(info);
        auto heading = Text(L"OPTIONS", 11, White(130));
        heading.CharacterSpacing(120);
        heading.Margin(Thickness{0, 4, 0, 0});
        detailsView.Children().Append(heading);
        StackPanel options;
        options.Spacing(6);
        OptionRow(options, ButtonChip(L"☰", Color{255, 70, 78, 92}), L"CPU counter (FPS and CPU use, top right)", &dCpu);
        OptionRow(options, ButtonChip(L"VIEW", Color{255, 70, 78, 92}), L"Export saves to the USB drive", nullptr);
        OptionRow(options, ButtonChip(L"X", Color{255, 40, 90, 170}), L"Import saves from the USB drive", nullptr);
        OptionRow(options, ButtonChip(L"RS", Color{255, 70, 78, 92}), L"Copy logs to the USB drive (click the right stick; for bug reports)", nullptr);
        detailsView.Children().Append(options);
        dStatusBox = Border();
        dStatusBox.CornerRadius(CornerRadius{6, 6, 6, 6});
        dStatusBox.Padding(Thickness{12, 8, 12, 8});
        dStatusBox.Visibility(Visibility::Collapsed);
        dStatus = Text(L"", 12);
        dStatus.TextWrapping(TextWrapping::Wrap);
        dStatus.TextTrimming(TextTrimming::None);
        dStatusBox.Child(dStatus);
        detailsView.Children().Append(dStatusBox);
        detailsView.Children().Append(Divider());
        Grid foot;
        ColumnDefinition f1, f2;
        f1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        f2.Width(GridLengthHelper::Auto());
        foot.ColumnDefinitions().Append(f1);
        foot.ColumnDefinitions().Append(f2);
        auto tip = Text(L"While playing, hold View + Menu for 3 seconds to come back here.", 11, White(140));
        tip.VerticalAlignment(VerticalAlignment::Center);
        foot.Children().Append(tip);
        auto close = Hint(L"B", L"CLOSE", Color{255, 170, 44, 44});
        Grid::SetColumn(close, 1);
        foot.Children().Append(close);
        detailsView.Children().Append(foot);
        return detailsView;
    }
    // The result of a save export or import: in the Details panel (green or red) and in the bottom line.
    void SaveStatus(const std::wstring& text, bool ok) {
        notice.Text(text);
        if (!dStatus) return;
        dStatus.Text(text);
        dStatusBox.Background(ok ? Solid(60, 40, 160, 80) : Solid(70, 200, 50, 50));
        dStatusBox.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
    }

    Border Card(const std::wstring& heading, TextBlock& big, TextBlock& sub, double width) {
        Border card;
        card.Width(width);
        card.Background(Solid(150, 12, 20, 34));
        card.BorderBrush(White(28));
        card.BorderThickness(Thickness{1, 1, 1, 1});
        card.CornerRadius(CornerRadius{8, 8, 8, 8});
        card.Padding(Thickness{14, 10, 14, 10});
        StackPanel stack;
        stack.Spacing(2);
        stack.Children().Append(Text(heading, 11, White(170)));
        big = Text(L"", 19);
        big.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        stack.Children().Append(big);
        sub = Text(L"", 11, White(170));
        stack.Children().Append(sub);
        card.Child(stack);
        return card;
    }

    LinearGradientBrush Gradient(Point from, Point to, std::initializer_list<std::pair<double, uint8_t>> stops) {
        LinearGradientBrush g;
        g.StartPoint(from);
        g.EndPoint(to);
        for (const auto& [offset, alpha] : stops) {
            GradientStop s;
            s.Offset(offset);
            s.Color(Color{alpha, 6, 11, 22});
            g.GradientStops().Append(s);
        }
        return g;
    }

    void Build() {
        Grid canvas;
        canvas.Width(kWidth);
        canvas.Height(kHeight);
        canvas.Background(Solid(255, 12, 20, 36));
        RectangleGeometry clip; clip.Rect(Rect{0, 0, (float)kWidth, (float)kHeight});
        canvas.Clip(clip);

        hero = Image();
        hero.Stretch(Stretch::UniformToFill);
        hero.HorizontalAlignment(HorizontalAlignment::Center);
        hero.VerticalAlignment(VerticalAlignment::Center);
        canvas.Children().Append(hero);
        Rectangle sideShade; sideShade.Fill(Gradient({0, 0}, {1, 0}, {{0.0, 225}, {0.45, 110}, {0.8, 20}}));
        canvas.Children().Append(sideShade);
        Rectangle lowShade; lowShade.Fill(Gradient({0, 0}, {0, 1}, {{0.0, 170}, {0.25, 40}, {0.55, 40}, {1.0, 245}}));
        canvas.Children().Append(lowShade);

        // Header
        Grid header;
        header.Height(52);
        header.VerticalAlignment(VerticalAlignment::Top);
        header.Margin(Thickness{kMarginX, 14, kMarginX, 0});
        StackPanel tabs;
        tabs.Orientation(Orientation::Horizontal);
        tabs.Spacing(14);
        tabs.Children().Append(Chip(L"LB"));
        tabGames = Text(L"Games", 18);
        tabAdd = Text(L"Add Game", 18);
        tabCompat = Text(L"Compatible", 18);
        tabGames.VerticalAlignment(VerticalAlignment::Center);
        tabAdd.VerticalAlignment(VerticalAlignment::Center);
        tabCompat.VerticalAlignment(VerticalAlignment::Center);
        tabs.VerticalAlignment(VerticalAlignment::Top);
        tabs.Height(28);
        tabs.Children().Append(tabGames);
        tabs.Children().Append(tabAdd);
        tabs.Children().Append(tabCompat);
        tabs.Children().Append(Chip(L"RB"));
        header.Children().Append(tabs);
        StackPanel right;
        right.Orientation(Orientation::Horizontal);
        right.HorizontalAlignment(HorizontalAlignment::Right);
        right.Spacing(16);
        clock = Text(L"", 14, White(220));
        clock.VerticalAlignment(VerticalAlignment::Center);
        right.Children().Append(clock);
        auto word = Text(L"XSTATION", 16);
        word.CharacterSpacing(220);
        word.FontWeight(Windows::UI::Text::FontWeights::Bold());
        word.VerticalAlignment(VerticalAlignment::Center);
        right.Children().Append(word);
        right.VerticalAlignment(VerticalAlignment::Top);
        right.Height(28);
        header.Children().Append(right);
        canvas.Children().Append(header);
        storage = Text(L"", 12, White(205));
        storage.HorizontalAlignment(HorizontalAlignment::Right);
        storage.VerticalAlignment(VerticalAlignment::Top);
        storage.Margin(Thickness{0, 44, kMarginX, 0});
        canvas.Children().Append(storage);

        // Games page
        gamesPage = Grid();
        gamesPage.Margin(Thickness{0, 62, 0, 44});
        Grid tileArea;
        tileArea.VerticalAlignment(VerticalAlignment::Top);
        tileArea.Height(122);
        tileRow = StackPanel();
        tileRow.Orientation(Orientation::Horizontal);
        tileRow.Spacing(kTileGap);
        tileRow.HorizontalAlignment(HorizontalAlignment::Left);
        tileRow.VerticalAlignment(VerticalAlignment::Top);
        tileShift = TranslateTransform();
        tileRow.RenderTransform(tileShift);
        tileArea.Children().Append(tileRow);
        tileLabel = Text(L"", 14);
        tileLabel.VerticalAlignment(VerticalAlignment::Bottom);
        tileLabel.HorizontalAlignment(HorizontalAlignment::Left);
        tileArea.Children().Append(tileLabel);
        gamesPage.Children().Append(tileArea);

        StackPanel info;
        info.VerticalAlignment(VerticalAlignment::Bottom);
        info.Margin(Thickness{kMarginX, 0, kMarginX, 18});
        info.Spacing(10);
        title = Text(L"", 44);
        title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        info.Children().Append(title);
        meta = Text(L"", 14, White(205));
        info.Children().Append(meta);
        copyRow = StackPanel();
        copyRow.Spacing(6);
        copyBarTrack = Bar(copyBar, 560, 8);
        copyBarTrack.HorizontalAlignment(HorizontalAlignment::Left);
        copyRow.Children().Append(copyBarTrack);
        copyLine = Text(L"", 13, White(225));
        copyRow.Children().Append(copyLine);
        copyRow.Visibility(Visibility::Collapsed);
        info.Children().Append(copyRow);

        StackPanel actions;
        actions.Orientation(Orientation::Horizontal);
        actions.Spacing(10);
        playButton = Border();
        playButton.CornerRadius(CornerRadius{6, 6, 6, 6});
        playButton.Width(190);
        playButton.Height(40);
        StackPanel playRow;
        playRow.Orientation(Orientation::Horizontal);
        playRow.Spacing(9);
        playRow.HorizontalAlignment(HorizontalAlignment::Center);
        playRow.VerticalAlignment(VerticalAlignment::Center);
        playIcon = Polygon();
        Windows::UI::Xaml::Media::PointCollection points;
        points.Append(Point{0, 0}); points.Append(Point{0, 14}); points.Append(Point{12, 7});
        playIcon.Points(points);
        playIcon.VerticalAlignment(VerticalAlignment::Center);
        playRow.Children().Append(playIcon);
        playLabel = Text(L"Play", 15);
        playLabel.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        playLabel.VerticalAlignment(VerticalAlignment::Center);
        playRow.Children().Append(playLabel);
        playButton.Child(playRow);
        actions.Children().Append(playButton);
        fpsButton = Border();
        fpsButton.Width(96);
        fpsButton.Height(40);
        fpsButton.CornerRadius(CornerRadius{6, 6, 6, 6});
        fpsButton.Background(White(38));
        fpsLabel = Text(L"60 FPS", 15);
        fpsLabel.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        fpsLabel.HorizontalAlignment(HorizontalAlignment::Center);
        fpsLabel.VerticalAlignment(VerticalAlignment::Center);
        fpsButton.Child(fpsLabel);
        actions.Children().Append(fpsButton);
        upscaleButton = Border();
        upscaleButton.Width(96);
        upscaleButton.Height(40);
        upscaleButton.CornerRadius(CornerRadius{6, 6, 6, 6});
        upscaleButton.Background(White(38));
        upscaleLabel = Text(L"FSR On", 15);
        upscaleLabel.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        upscaleLabel.HorizontalAlignment(HorizontalAlignment::Center);
        upscaleLabel.VerticalAlignment(VerticalAlignment::Center);
        upscaleButton.Child(upscaleLabel);
        upscaleButton.Visibility(Visibility::Collapsed);
        actions.Children().Append(upscaleButton);
        Border more;
        more.Width(40); more.Height(40);
        more.CornerRadius(CornerRadius{20, 20, 20, 20});
        more.Background(White(38));
        auto moreText = Text(L"•••", 11);
        moreText.HorizontalAlignment(HorizontalAlignment::Center);
        moreText.VerticalAlignment(VerticalAlignment::Center);
        more.Child(moreText);
        actions.Children().Append(more);
        info.Children().Append(actions);

        StackPanel cards;
        cards.Orientation(Orientation::Horizontal);
        cards.Spacing(10);
        cards.Children().Append(Card(L"Your time", cardTime, cardTimeSub, 200));
        cards.Children().Append(Card(L"Performance on Xbox", cardSpeed, cardSpeedSub, 220));
        cards.Children().Append(Card(L"Status", cardStatus, cardStatusSub, 200));
        cards.Children().Append(Card(L"Installed", cardSize, cardSizeSub, 200));
        info.Children().Append(cards);
        gamesPage.Children().Append(info);
        canvas.Children().Append(gamesPage);

        // Add Game page (layout B, 2026-11-10): a progress row on top; the USB drive's games as rows with their art on the
        // left; "From your PC" (Wi-Fi) always on the right.
        addPage = StackPanel();
        addPage.Margin(Thickness{kMarginX, 66, kMarginX, 50});
        addPage.Spacing(14);
        addPage.Visibility(Visibility::Collapsed);
        progressRow = Border();
        progressRow.CornerRadius(CornerRadius{10, 10, 10, 10});
        progressRow.Background(Solid(64, 40, 130, 230));
        progressRow.Padding(Thickness{12, 10, 16, 10});
        progressRow.Visibility(Visibility::Collapsed);
        {
            Grid g;
            ColumnDefinition c0, c1, c2;
            c0.Width(GridLengthHelper::FromPixels(66));
            c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            c2.Width(GridLengthHelper::Auto());
            g.ColumnDefinitions().Append(c0);
            g.ColumnDefinitions().Append(c1);
            g.ColumnDefinitions().Append(c2);
            progressArt = Border();
            progressArt.Width(52);
            progressArt.Height(52);
            progressArt.CornerRadius(CornerRadius{8, 8, 8, 8});
            progressArt.Background(White(30));
            g.Children().Append(progressArt);
            StackPanel mid;
            mid.Spacing(8);
            mid.VerticalAlignment(VerticalAlignment::Center);
            mid.Margin(Thickness{2, 0, 18, 0});
            progressTitle = Text(L"", 16);
            mid.Children().Append(progressTitle);
            usbBarTrack = Bar(usbBar, 0, 8);
            mid.Children().Append(usbBarTrack);
            Grid::SetColumn(mid, 1);
            g.Children().Append(mid);
            progressRight = Text(L"", 13, White(175));
            progressRight.TextAlignment(TextAlignment::Right);
            progressRight.TextTrimming(TextTrimming::None);
            progressRight.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(progressRight, 2);
            g.Children().Append(progressRight);
            progressRow.Child(g);
        }
        addPage.Children().Append(progressRow);
        {
            Grid columns;
            ColumnDefinition left, right;
            left.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            right.Width(GridLengthHelper::FromPixels(370));
            columns.ColumnDefinitions().Append(left);
            columns.ColumnDefinitions().Append(right);
            columns.ColumnSpacing(22);
            StackPanel usbSide;
            usbSide.Spacing(8);
            usbHeader = Text(L"USB drive", 14, White(165));
            usbSide.Children().Append(usbHeader);
            usbList = StackPanel();
            usbList.Spacing(6);
            usbSide.Children().Append(usbList);
            columns.Children().Append(usbSide);
            StackPanel pcSide;
            pcSide.Spacing(8);
            pcSide.Children().Append(Text(L"From your PC", 14, White(165)));
            Border card;
            card.CornerRadius(CornerRadius{12, 12, 12, 12});
            card.Background(White(16));
            card.Padding(Thickness{18, 14, 18, 16});
            StackPanel stack;
            stack.Spacing(2);
            stack.Children().Append(Text(L"On a PC on the same network, open", 13, White(165)));
            wifiAddressText = Text(L"Starting\u2026", 22);
            wifiAddressText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            wifiAddressText.Margin(Thickness{0, 2, 0, 12});
            stack.Children().Append(wifiAddressText);
            stack.Children().Append(Text(L"Pairing code", 13, White(165)));
            wifiCodeText = Text(L"", 40);
            wifiCodeText.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            wifiCodeText.CharacterSpacing(300);
            wifiCodeText.Margin(Thickness{0, 0, 0, 10});
            stack.Children().Append(wifiCodeText);
            Border divider;
            divider.Height(1);
            divider.Background(White(32));
            divider.Margin(Thickness{0, 0, 0, 8});
            stack.Children().Append(divider);
            stack.Children().Append(Text(L"Status", 13, White(165)));
            wifiStatusText = Text(L"Waiting for a PC", 15);
            wifiStatusText.TextWrapping(TextWrapping::Wrap);
            wifiStatusText.TextTrimming(TextTrimming::None);
            stack.Children().Append(wifiStatusText);
            auto help = Text(L"Send a game's PKG (the Xbox converts it as it arrives) or a prepared game folder. It shows up in Games when it's done.", 12, White(140));
            help.TextWrapping(TextWrapping::Wrap);
            help.TextTrimming(TextTrimming::None);
            help.Margin(Thickness{0, 12, 0, 0});
            stack.Children().Append(help);
            card.Child(stack);
            pcSide.Children().Append(card);
            Grid::SetColumn(pcSide, 1);
            columns.Children().Append(pcSide);
            addPage.Children().Append(columns);
        }
        // Kept for older code paths (logs, the import list); not on the page.
        importList = StackPanel();
        usbCopyLine = Text(L"", 13);
        wifiText = Text(L"", 13);
        wifiProgress = Text(L"", 13);
        usbText = Text(L"", 13);
        canvas.Children().Append(addPage);

        // Compatible page (2026-11-18): every game this package has an engine for (RefreshCompat fills the rows).
        compatPage = StackPanel();
        compatPage.Margin(Thickness{kMarginX, 66, kMarginX, 50});
        compatPage.Spacing(8);
        compatPage.Visibility(Visibility::Collapsed);
        compatPage.Children().Append(Text(L"Games XStation can play", 14, White(165)));
        compatList = StackPanel();
        compatList.Spacing(6);
        compatPage.Children().Append(compatList);
        {
            auto note = Text(L"Each game needs the exact version listed. A copy from another region works too when its game code is "
                             L"the same, and XStation checks this when you add the game. Speeds were measured on an Xbox Series X.",
                             12, White(150));
            note.TextWrapping(TextWrapping::Wrap);
            note.TextTrimming(TextTrimming::None);
            note.Margin(Thickness{0, 6, 0, 0});
            compatPage.Children().Append(note);
        }
        canvas.Children().Append(compatPage);

        // Hints
        Grid hints;
        hints.Height(36);
        hints.VerticalAlignment(VerticalAlignment::Bottom);
        hints.Margin(Thickness{kMarginX, 0, kMarginX, 6});
        StackPanel hintRight;
        hintRight.Orientation(Orientation::Horizontal);
        hintRight.HorizontalAlignment(HorizontalAlignment::Right);
        hintRight.Spacing(18);
        hintRight.Children().Append(Hint(L"X", L"DELETE", Color{255, 40, 90, 170}));
        hintRight.Children().Append(Hint(L"Y", L"DETAILS", Color{255, 196, 150, 20}));
        hintRight.Children().Append(Hint(L"◄", L"PREV", Color{255, 70, 78, 92}));
        hintRight.Children().Append(Hint(L"►", L"NEXT", Color{255, 70, 78, 92}));
        hintFps = Hint(L"▲", L"30 / 60 FPS", Color{255, 70, 78, 92});
        hintRight.Children().Append(hintFps);
        hintUpscale = Hint(L"\u25BC", L"FSR ON / OFF", Color{255, 70, 78, 92});
        hintUpscale.Visibility(Visibility::Collapsed);
        hintRight.Children().Append(hintUpscale);
        hintRight.Children().Append(Hint(L"A", L"PLAY", Color{255, 40, 140, 70}));
        hintRight.Children().Append(Hint(L"B", L"BACK", Color{255, 170, 44, 44}));
        hints.Children().Append(hintRight);
        hintsGames = hintRight;
        hintsAdd = StackPanel();
        hintsAdd.Orientation(Orientation::Horizontal);
        hintsAdd.HorizontalAlignment(HorizontalAlignment::Right);
        hintsAdd.Spacing(18);
        hintsAdd.Visibility(Visibility::Collapsed);
        hintsAdd.Children().Append(Hint(L"X", L"RESCAN", Color{255, 40, 90, 170}));
        hintsAdd.Children().Append(Hint(L"B", L"BACK", Color{255, 170, 44, 44}));
        hints.Children().Append(hintsAdd);
        hintsCompat = StackPanel();
        hintsCompat.Orientation(Orientation::Horizontal);
        hintsCompat.HorizontalAlignment(HorizontalAlignment::Right);
        hintsCompat.Spacing(18);
        hintsCompat.Visibility(Visibility::Collapsed);
        hintsCompat.Children().Append(Hint(L"B", L"BACK", Color{255, 170, 44, 44}));
        hints.Children().Append(hintsCompat);
        notice = Text(L"", 12, White(220));
        notice.VerticalAlignment(VerticalAlignment::Center);
        notice.MaxWidth(620);
        notice.HorizontalAlignment(HorizontalAlignment::Left);
        hints.Children().Append(notice);
        canvas.Children().Append(hints);

        // Details panel
        details = Border();
        details.Width(680);
        details.HorizontalAlignment(HorizontalAlignment::Center);
        details.VerticalAlignment(VerticalAlignment::Center);
        details.Background(Solid(245, 14, 22, 38));
        details.BorderBrush(White(50));
        details.BorderThickness(Thickness{1, 1, 1, 1});
        details.CornerRadius(CornerRadius{10, 10, 10, 10});
        details.Padding(Thickness{26, 22, 26, 22});
        details.Visibility(Visibility::Collapsed);
        detailsText = Text(L"", 14);
        detailsText.TextWrapping(TextWrapping::Wrap);
        detailsText.TextTrimming(TextTrimming::None);
        {
            Grid both;
            both.Children().Append(detailsText);
            both.Children().Append(BuildDetailsView());
            details.Child(both);
        }
        canvas.Children().Append(details);

        toast = Border();
        toast.Width(600);
        toast.HorizontalAlignment(HorizontalAlignment::Center);
        toast.VerticalAlignment(VerticalAlignment::Top);
        toast.Margin(Thickness{0, 74, 0, 0});
        toast.Background(Solid(250, 40, 18, 22));
        toast.BorderBrush(Solid(255, 232, 72, 72));
        toast.BorderThickness(Thickness{4, 1, 1, 1});
        toast.CornerRadius(CornerRadius{10, 10, 10, 10});
        toast.Padding(Thickness{20, 14, 20, 16});
        toast.Visibility(Visibility::Collapsed);
        toastMove = Media::TranslateTransform();
        toast.RenderTransform(toastMove);
        {
            Grid row;
            ColumnDefinition iconColumn; iconColumn.Width(GridLengthHelper::FromPixels(44));
            ColumnDefinition textColumn; textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            row.ColumnDefinitions().Append(iconColumn);
            row.ColumnDefinitions().Append(textColumn);
            Border badge;
            badge.Width(32);
            badge.Height(32);
            badge.CornerRadius(CornerRadius{16, 16, 16, 16});
            badge.Background(Solid(255, 232, 72, 72));
            badge.VerticalAlignment(VerticalAlignment::Top);
            badge.HorizontalAlignment(HorizontalAlignment::Left);
            TextBlock mark = Text(L"!", 20);
            mark.FontWeight(Windows::UI::Text::FontWeights::Bold());
            mark.HorizontalAlignment(HorizontalAlignment::Center);
            mark.VerticalAlignment(VerticalAlignment::Center);
            badge.Child(mark);
            row.Children().Append(badge);
            StackPanel words;
            words.Spacing(4);
            toastTitle = Text(L"", 18);
            toastTitle.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            toastBody = Text(L"", 14, White(225));
            toastBody.TextWrapping(TextWrapping::Wrap);
            toastBody.TextTrimming(TextTrimming::None);
            TextBlock close = Text(L"Press B to close", 12, White(150));
            close.Margin(Thickness{0, 4, 0, 0});
            words.Children().Append(toastTitle);
            words.Children().Append(toastBody);
            words.Children().Append(close);
            Grid::SetColumn(words, 1);
            row.Children().Append(words);
            toast.Child(row);
        }
        canvas.Children().Append(toast);

        Viewbox view;
        view.Stretch(Stretch::Uniform);
        view.Child(canvas);
        Grid root;
        root.Background(Solid(255, 0, 0, 0));
        root.Children().Append(view);
        Window::Current().Content(root);
    }

    // BitmapImage only reads package and app-data URIs, so the bytes are handed to it.
    fire_and_forget LoadBytes(BitmapImage image, std::vector<uint8_t> bytes) {
        auto keep = get_strong();
        Streams::InMemoryRandomAccessStream stream;
        Streams::DataWriter writer(stream);
        writer.WriteBytes(array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
        co_await writer.StoreAsync();
        writer.DetachStream();
        stream.Seek(0);
        try { co_await image.SetSourceAsync(stream); } catch (...) {}
    }

    fire_and_forget LoadFile(BitmapImage image, std::wstring path) {
        auto keep = get_strong();
        std::ifstream file(path, std::ios::binary);
        if (!file) co_return;
        std::string bytes((std::istreambuf_iterator<char>(file)), {});
        Streams::InMemoryRandomAccessStream stream;
        Streams::DataWriter writer(stream);
        writer.WriteBytes(array_view<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size()));
        co_await writer.StoreAsync();
        writer.DetachStream();
        stream.Seek(0);
        co_await image.SetSourceAsync(stream);
    }

    std::vector<Mount> ReadMounts() {
        std::vector<Mount> out;
        std::ifstream file(local + L"\\mounts.txt");
        std::string text;
        while (std::getline(file, text)) {
            const std::wstring line = FromUtf8(text);
            const size_t a = line.find(L'\t');
            if (a == std::wstring::npos) continue;
            const size_t b = line.find(L'\t', a + 1);
            Mount m{line.substr(0, a), line.substr(a + 1, b == std::wstring::npos ? std::wstring::npos : b - a - 1),
                    b == std::wstring::npos ? L"" : line.substr(b + 1)};
            while (!m.title.empty() && (m.title.back() == L'\r' || m.title.back() == L' ')) m.title.pop_back();
            while (!m.folder.empty() && (m.folder.back() == L'\r' || m.folder.back() == L' ')) m.folder.pop_back();
            if (!m.id.empty() && !m.folder.empty()) out.push_back(m);
        }
        return out;
    }
    bool WriteMounts(const std::vector<Mount>& mounts) {
        std::ofstream file(local + L"\\mounts.txt", std::ios::trunc | std::ios::binary);
        for (const auto& m : mounts) file << ToUtf8(m.id) << '\t' << ToUtf8(m.folder) << '\t' << ToUtf8(m.title) << '\n';
        return (bool)file.flush();
    }

    void Rescan() {
        const std::wstring keep = selected < games.size() ? games[selected].id : L"";
        games.clear();
        ScanGames(package + L"\\Games", package, local, games);
        const size_t before = games.size();
        ScanGames(dataRoot + L"\\Games", package, local, games);
        for (size_t i = before; i < games.size(); ++i) games[i].removable = true;
        // Mounted games (a copy on the Xbox wins: its id is skipped here).
        auto mounts = ReadMounts();
        bool retitled = false;
        for (auto& m : mounts) {
            bool installed = false;
            for (const auto& g : games) if (g.id == m.id) installed = true;
            if (installed) continue;
            // XStation (2026-11-11): the drive is read by StartMountScan on a worker; here only its last result.
            std::vector<Game> found;
            bool checked = false;
            {
                std::scoped_lock lock{mountScanMutex};
                if (auto it = mountFound.find(m.id); it != mountFound.end()) found.push_back(it->second);
                checked = mountChecked.count(m.id) != 0;
            }
            bool present = false;
            for (auto& g : found) {
                if (g.id != m.id) continue;
                g.mounted = true;
                if (g.title != m.title && g.title != g.id) { m.title = g.title; retitled = true; }
                games.push_back(g);
                present = true;
                break;
            }
            if (!present) {
                Game g;
                g.id = m.id;
                g.title = m.title.empty() ? m.id : m.title;
                g.folder = m.folder;
                g.mounted = true;
                g.usbMissing = checked; // not read yet: listed from mounts.txt until the worker has looked
                g.hasEngine = EngineExists(package, m.id);
                ReadStats(local + L"\\Titles\\" + m.id + L"\\stats.txt", g);
                games.push_back(g);
            }
        }
        if (retitled) WriteMounts(mounts);
        StartMountScan(mounts);
        std::stable_sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
            const bool ra = a.hasBoot && a.hasEngine, rb = b.hasBoot && b.hasEngine;
            if (a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
            if (ra != rb) return ra;
            return a.title < b.title;
        });
        for (size_t i = 0; i < games.size(); ++i) if (!keep.empty() && games[i].id == keep) selected = i;
        if (selected >= games.size()) selected = games.empty() ? 0 : games.size() - 1;
        BuildTiles();
    }

    void AskDelete() {
        if (tab != 0 || detailsOpen || confirmOpen || deleting || launching || games.empty()) return;
        const Game& g = games[selected];
        if (CopyRunning() && g.id == CopyId()) { notice.Text(L"This game is still copying from the USB drive."); return; }
        if (!g.removable && !g.mounted) { notice.Text(L"This game is inside the app's own folder, so it can't be deleted here."); return; }
        confirmKind = 0;
        confirmOpen = true;
        Refresh(false);
    }

    void ConfirmDelete() {
        if (!confirmOpen || selected >= games.size()) return;
        confirmOpen = false;
        const Game g = games[selected];
        if (g.mounted) { // unmount: only the library entry goes, the drive is not touched
            auto mounts = ReadMounts();
            mounts.erase(std::remove_if(mounts.begin(), mounts.end(), [&](const Mount& m) { return m.id == g.id; }), mounts.end());
            if (!WriteMounts(mounts)) { notice.Text(L"Couldn't save the change."); Refresh(false); return; }
            Rescan();
            Refresh();
            notice.Text(L"Unmounted " + g.title + L". The game is still on the USB drive, and your saves were kept.");
            return;
        }
        // Only ever a direct child of our own Games folder, named like a title ID.
        const std::wstring expected = dataRoot + L"\\Games\\" + g.id;
        bool safe = g.removable && g.folder == expected && !g.id.empty();
        for (const wchar_t c : g.id) if (!iswalnum(c)) safe = false;
        if (!safe) { notice.Text(L"That game can't be deleted."); Refresh(false); return; }
        deleting = true;
        deleteFinished = false;
        deletedFiles = 0;
        deleteFailures = 0;
        deletingTitle = g.title;
        std::thread([this, keep = get_strong(), folder = g.folder] {
            DeleteTree(folder, deletedFiles, deleteFailures);
            deleteFinished = true;
        }).detach();
        Refresh(false);
    }

    void ShowStorage() {
        std::wstring line = L"Games  " + DriveLine(dataRoot);
        if (towupper(local[0]) != towupper(dataRoot[0])) line += L"     App data  " + DriveLine(local);
        storage.Text(line);
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::wofstream log(dataRoot + L"\\xstation-wifi.log", std::ios::app);
            log << line << L"\n";
        }
    }

    void BuildTiles() {
        tileRow.Children().Clear();
        tileBars.clear();
        for (size_t i = 0; i < games.size(); ++i) {
            const Game& g = games[i];
            Border tile;
            tile.CornerRadius(CornerRadius{10, 10, 10, 10});
            tile.Background(Solid(255, 30, 40, 58));
            if (!g.icon.empty()) {
                BitmapImage image;
                image.DecodePixelWidth(184);
                LoadFile(image, g.uriBase + g.icon);
                ImageBrush art;
                art.ImageSource(image);
                art.Stretch(Stretch::UniformToFill);
                tile.Background(art);
            }
            Grid over;
            Grid bar{nullptr};
            if (g.mounted) {
                Border chip;
                chip.Background(Solid(220, 10, 16, 28));
                chip.CornerRadius(CornerRadius{4, 4, 4, 4});
                chip.Padding(Thickness{5, 1, 5, 1});
                chip.Margin(Thickness{0, 0, 5, 5});
                chip.HorizontalAlignment(HorizontalAlignment::Right);
                chip.VerticalAlignment(VerticalAlignment::Bottom);
                chip.Child(Text(g.usbMissing ? L"USB \u2715" : L"USB", 10));
                over.Children().Append(chip);
                if (g.usbMissing) tile.Opacity(0.5);
            }
            if (g.copying) {
                Border track = Bar(bar, 0, 6);
                track.VerticalAlignment(VerticalAlignment::Bottom);
                track.Margin(Thickness{6, 0, 6, 6});
                over.Children().Append(track);
                SetBar(bar, g.id == CopyId() && copyTotal ? double(copyDone) / double(copyTotal) : 0.0);
            }
            if (g.mounted || g.copying) tile.Child(over);
            tileBars.push_back(bar);
            tileRow.Children().Append(tile);
        }
    }

    void ShowHero(const Game& g) {
        if (g.hero.empty()) { hero.Source(nullptr); return; }
        BitmapImage image;
        image.DecodePixelWidth(1280);
        LoadFile(image, g.uriBase + g.hero);
        hero.Source(image);
        hero.Opacity(0);
        Anim::DoubleAnimation fade;
        fade.From(0.0);
        fade.To(1.0);
        fade.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(260)));
        Anim::Storyboard board;
        board.Children().Append(fade);
        Anim::Storyboard::SetTarget(fade, hero);
        Anim::Storyboard::SetTargetProperty(fade, L"Opacity");
        board.Begin();
    }

    // The Compatible tab's rows: name, the exact version and copy, speed and status on the Xbox, and whether it's in the
    // library (under its own ID, or another region's copy that runs with this engine).
    void RefreshCompat() {
        compatList.Children().Clear();
        size_t shown = 0;
        for (const auto& c : kCompatible) {
            if (!EngineExists(package, c.id)) continue;
            ++shown;
            std::wstring speed = L"Not tested", status = L"";
            for (const auto& t : kTested) if (!wcscmp(t.id, c.id)) { speed = t.speed; status = t.status; }
            std::wstring have;
            for (const auto& g : games) {
                if (GameEngineId(g.id) != c.id) continue;
                have = g.id == c.id ? L"In your library" : L"In your library (" + g.id + L")";
                break;
            }
            Border row;
            row.CornerRadius(CornerRadius{10, 10, 10, 10});
            row.Background(White(16));
            row.Padding(Thickness{16, 10, 16, 10});
            Grid g;
            ColumnDefinition c0, c1, c2;
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            c1.Width(GridLengthHelper::FromPixels(190));
            c2.Width(GridLengthHelper::FromPixels(190));
            g.ColumnDefinitions().Append(c0);
            g.ColumnDefinitions().Append(c1);
            g.ColumnDefinitions().Append(c2);
            StackPanel left;
            left.Spacing(2);
            left.VerticalAlignment(VerticalAlignment::Center);
            auto name = Text(c.name, 17);
            name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            left.Children().Append(name);
            left.Children().Append(Text(L"Version " + std::wstring(c.version) + L"  ·  " + c.region + L" copy (" + c.id + L")", 12, White(170)));
            g.Children().Append(left);
            StackPanel mid;
            mid.Spacing(2);
            mid.VerticalAlignment(VerticalAlignment::Center);
            mid.Children().Append(Text(speed, 15));
            mid.Children().Append(Text(status.empty() ? L"" : status, 12, White(170)));
            Grid::SetColumn(mid, 1);
            g.Children().Append(mid);
            if (!have.empty()) {
                Border chip;
                chip.CornerRadius(CornerRadius{12, 12, 12, 12});
                chip.Background(Solid(110, 40, 140, 70));
                chip.Padding(Thickness{10, 4, 10, 4});
                chip.HorizontalAlignment(HorizontalAlignment::Right);
                chip.VerticalAlignment(VerticalAlignment::Center);
                chip.Child(Text(have, 12));
                Grid::SetColumn(chip, 2);
                g.Children().Append(chip);
            }
            row.Child(g);
            compatList.Children().Append(row);
        }
        if (!shown) compatList.Children().Append(Text(L"This XStation has no game engines.", 15, White(200)));
    }

    void Refresh(bool heroChanged = true) {
        tabGames.Foreground(tab == 0 ? White() : White(130));
        tabAdd.Foreground(tab == 1 ? White() : White(130));
        tabGames.FontWeight(tab == 0 ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal());
        tabAdd.FontWeight(tab == 1 ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal());
        tabCompat.Foreground(tab == 2 ? White() : White(130));
        tabCompat.FontWeight(tab == 2 ? Windows::UI::Text::FontWeights::SemiBold() : Windows::UI::Text::FontWeights::Normal());
        gamesPage.Visibility(tab == 0 ? Visibility::Visible : Visibility::Collapsed);
        addPage.Visibility(tab == 1 ? Visibility::Visible : Visibility::Collapsed);
        compatPage.Visibility(tab == 2 ? Visibility::Visible : Visibility::Collapsed);
        details.Visibility(detailsOpen || confirmOpen ? Visibility::Visible : Visibility::Collapsed);
        detailsText.Visibility(confirmOpen ? Visibility::Visible : Visibility::Collapsed);
        if (detailsView) detailsView.Visibility(detailsOpen && !confirmOpen ? Visibility::Visible : Visibility::Collapsed);
        if (confirmOpen && confirmKind == 5) {
            detailsText.Text(L"XStation " + updateTag + L" is out. You have " + updateMine + L".\n\nWhat's new:\n" + updateNotes +
                             L"\n\nTo update, download the .msix from the release page and install it with Xbox Device Portal "
                             L"(Apps, Add), like before. Your games and saves stay.\n" + updateUrl +
                             L"\n\nA  Open the page          B  Later");
        } else if (confirmOpen && confirmKind == 4 && selected < games.size() && importPick < importChoices.size()) {
            detailsText.Text(L"Import saves for " + games[selected].title + L"?\n\nFrom the USB drive:  XStation\\Saves\\" + games[selected].id +
                             L"\\" + importChoices[importPick] + L"\n(" + std::to_wstring(importPick + 1) + L" of " +
                             std::to_wstring(importChoices.size()) + L", newest first" +
                             (importChoices.size() > 1 ? L"; press X for the next one)" : L")") +
                             L"\n\nThe game's current saves are replaced. They are kept first on the Xbox in Titles\\" + games[selected].id +
                             L"\\save-backups, so nothing is lost.\n\nA  Import          B  Cancel");
        } else if (confirmOpen && confirmKind == 3) {
            const std::wstring name = shrinkOfferPath.substr(shrinkOfferPath.find_last_of(L'\\') + 1);
            detailsText.Text(L"Use up the PKG to convert it?\n\n" + name +
                             L"\n\nThe drive doesn't have room for the PKG and the game together: the normal way needs " + FormatBytes(shrinkOfferNormal) +
                             L" free, and " + FormatBytes(shrinkOfferFree) + L" is free.\n\nXStation can use the PKG up instead: it writes the game from the "
                             L"end of the PKG backwards and shortens the PKG as it goes, so only " + FormatBytes(shrinkOfferNeed) +
                             L" has to be free. When the game is done, the PKG is gone (to convert again you'd need the PKG again). "
                             L"If it stops part way, convert the same PKG again to finish it.\n\nA  Use it up          B  Cancel");
        } else if (confirmOpen && confirmKind == 2) {
            std::wstring names;
            uint64_t bytes = 0;
            for (const auto& p : convertedPkgs) {
                WIN32_FILE_ATTRIBUTE_DATA a{};
                uint64_t size = 0;
                if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
                bytes += size;
                names += L"\n" + p.substr(p.find_last_of(L'\\') + 1) + L"   " + FormatBytes(size);
            }
            detailsText.Text(std::wstring(convertedPkgs.size() > 1 ? L"Delete the PKGs you converted?\n" : L"Delete the PKG you converted?\n") + names +
                             L"\n\nThe game is converted and checked, so you don't need the PKG to play it. Deleting it frees " +
                             FormatBytes(bytes) + L" on the USB drive. This can't be undone: to convert again you'd need the PKG again." +
                             L"\n\nA  Delete          B  Keep it");
        } else if (confirmOpen && confirmKind == 1 && selected < games.size()) {
            detailsText.Text(L"Turn on 60 FPS with FSR for " + games[selected].title + L"?\n\n"
                             L"FSR frame generation adds a made-up picture between every two of the game's frames, so it "
                             L"looks smoother (about 50\u201360 FPS). It uses about 110 MB more memory, and the Xbox has "
                             L"little to spare, so a crash from running out of memory gets more likely. The game itself runs "
                             L"a little slower (about 26\u201328 instead of 30), controls still feel like 30 FPS, and fast "
                             L"movement can smear.\n\nA  Turn on          B  Cancel");
        } else if (confirmOpen && selected < games.size() && games[selected].mounted) {
            detailsText.Text(L"Unmount " + games[selected].title + L"?\n\nIt leaves your library. The game files stay on the USB drive, "
                             L"and your saves are kept. Mount it again from Add Game whenever you like.\n\nA  Unmount          B  Cancel");
        } else if (confirmOpen && selected < games.size()) {
            detailsText.Text(L"Delete " + games[selected].title + L"?\n\nThis removes the game files from " + games[selected].folder +
                             L" and frees " + FormatBytes(games[selected].bytes) + L". Your saves are kept. To play it again you'll have to send the game again.\n\nA  Delete          B  Cancel");
        }
        hintsGames.Visibility(tab == 0 ? Visibility::Visible : Visibility::Collapsed);
        hintsAdd.Visibility(tab == 1 ? Visibility::Visible : Visibility::Collapsed);
        hintsCompat.Visibility(tab == 2 ? Visibility::Visible : Visibility::Collapsed);
        if (tab == 2) {
            hero.Opacity(0.25);
            RefreshCompat();
            return;
        }
        if (tab == 1) {
            hero.Opacity(0.25);
            StartWifi();
            RefreshImport();
            RefreshUsb();
            if (!usbScannedOnce) ScanUsb();
            return;
        }
        hero.Opacity(1.0);
        if (games.empty()) {
            hero.Source(nullptr);
            title.Text(L"No games yet");
            meta.Text(L"Press RB to see how to add one.");
            tileLabel.Text(L"");
            fpsButton.Visibility(Visibility::Collapsed);
            hintFps.Visibility(Visibility::Collapsed);
            upscaleButton.Visibility(Visibility::Collapsed);
            hintUpscale.Visibility(Visibility::Collapsed);
            SetPlay(false, L"Play");
            SetCards(L"—", L"", L"—", L"", L"—", L"", L"—", L"");
            return;
        }
        double x = kMarginX, selectedX = kMarginX;
        const auto tiles = tileRow.Children();
        for (uint32_t i = 0; i < tiles.Size(); ++i) {
            auto tile = tiles.GetAt(i).as<Border>();
            const bool on = i == selected;
            const double size = on ? kTileSelected : kTile;
            tile.Width(size);
            tile.Height(size);
            tile.BorderBrush(on ? White() : White(0));
            tile.BorderThickness(on ? Thickness{3, 3, 3, 3} : Thickness{0, 0, 0, 0});
            tile.Opacity(on ? 1.0 : 0.82);
            tile.Margin(Thickness{0, on ? 0.0 : (kTileSelected - kTile) / 2, 0, 0});
            if (on) selectedX = x;
            x += size + kTileGap;
        }
        // Keep the selected tile on screen, near the left edge once the row is long.
        const double shift = selectedX + kTileSelected + kMarginX > kWidth * 0.7 ? kWidth * 0.7 - selectedX - kTileSelected - kMarginX : 0.0;
        tileRow.Margin(Thickness{kMarginX, 0, 0, 0});
        tileShift.X(shift);
        tileLabel.Margin(Thickness{selectedX + shift, 0, 0, 0});

        const Game& g = games[selected];
        tileLabel.Text(g.title);
        title.Text(g.title);
        meta.Text(FormatAgo(g.lastPlayed) + L"  ·  " + FormatHours(g.seconds) + L" played" + (g.mounted ? L"  ·  On a USB drive" : L""));
        const bool copyingThis = g.copying || (CopyRunning() && g.id == CopyId());
        const bool ready = g.hasBoot && g.hasEngine && !g.usbMissing && !copyingThis;
        if (g.usbMissing) SetPlay(false, L"Plug in the USB drive");
        else if (copyingThis) SetPlay(false, CopyRunning() && g.id == CopyId() ? L"Copying\u2026" : L"Copy not finished");
        else SetPlay(ready, ready ? L"Play" : (g.hasEngine ? L"Not prepared" : L"No engine yet"));
        copyRow.Visibility(copyingThis ? Visibility::Visible : Visibility::Collapsed);
        if (copyingThis && !(CopyRunning() && g.id == CopyId())) {
            SetBar(copyBar, 0);
            copyLine.Text(L"This copy didn't finish. Copy the game again from the USB drive: files already copied are kept.");
        }
        const bool fpsChoice = HasFpsChoice(g.id);
        fpsButton.Visibility(fpsChoice ? Visibility::Visible : Visibility::Collapsed);
        hintFps.Visibility(fpsChoice ? Visibility::Visible : Visibility::Collapsed);
        fpsLabel.Text(std::to_wstring(g.fps) + L" FPS" + (g.fsr ? L" (FSR)" : L""));
        fpsButton.Width(g.fsr ? 150 : 96);
        const bool upscaleChoice = HasFsrUpscale(g.id);
        upscaleButton.Visibility(upscaleChoice ? Visibility::Visible : Visibility::Collapsed);
        hintUpscale.Visibility(upscaleChoice ? Visibility::Visible : Visibility::Collapsed);
        upscaleLabel.Text(g.upscale ? L"FSR On" : L"FSR Off");

        std::wstring speed = L"Not tested", status = L"Unknown", statusSub = L"Ready to start";
        for (const auto& t : kTested) {
            if (GameEngineId(g.id) == t.id) { speed = t.speed; status = t.status; statusSub = L"Ready to start"; }
        }
        if (!g.hasEngine) { status = L"No engine"; statusSub = L"This game can't start yet"; }
        else if (g.usbMissing) { status = L"USB drive missing"; statusSub = L"Plug it in to play"; }
        else if (copyingThis) { status = L"Copying"; statusSub = L"Wait for the copy to finish"; }
        else if (!g.hasBoot) { status = L"Not prepared"; statusSub = L"Delete it and add its PKG again"; }
        else if (g.mounted) statusSub = L"Plays from the USB drive";
        SetCards(FormatHours(g.seconds), FormatAgo(g.lastPlayed), speed, speed == L"Not tested" ? L"No results recorded" : L"Tested on Xbox Series X", status, statusSub,
                 FormatBytes(g.bytes), g.id + (g.version.empty() ? L"" : L"  ·  v" + g.version));
        if (heroChanged) ShowHero(g);
        if (detailsOpen && detailsView) {
            dTitle.Text(g.title);
            dSub.Text(g.id + (g.version.empty() ? L"" : L"   \u00B7   Version " + g.version) + L"   \u00B7   " + FormatBytes(g.bytes) +
                      (g.mounted ? L"   \u00B7   On a USB drive" : L""));
            dFolder.Text(g.folder);
            dSaves.Text(SaveFolder(local, g.id));
            dEngine.Text(g.hasEngine ? L"Included" : L"Not included");
            dEngine.Foreground(g.hasEngine ? White(230) : Solid(255, 240, 120, 110));
            dBoot.Text(g.hasBoot ? L"Found" : L"Missing");
            dBoot.Foreground(g.hasBoot ? White(230) : Solid(255, 240, 120, 110));
            dCpu.Text(g.cpu ? L"On" : L"Off");
            dCpu.Foreground(g.cpu ? Solid(255, 110, 220, 140) : White(150));
        }
    }

    void SetPlay(bool enabled, const std::wstring& label) {
        playLabel.Text(label);
        playButton.Background(enabled ? White() : White(46));
        playLabel.Foreground(enabled ? Solid(255, 10, 16, 28) : White(170));
        playIcon.Fill(Solid(255, 10, 16, 28));
        playIcon.Visibility(enabled ? Visibility::Visible : Visibility::Collapsed);
    }

    void SetCards(const std::wstring& a, const std::wstring& a2, const std::wstring& b, const std::wstring& b2, const std::wstring& c,
                  const std::wstring& c2, const std::wstring& d, const std::wstring& d2) {
        cardTime.Text(a); cardTimeSub.Text(a2); cardSpeed.Text(b); cardSpeedSub.Text(b2);
        cardStatus.Text(c); cardStatusSub.Text(c2); cardSize.Text(d); cardSizeSub.Text(d2);
    }

    void StartWifi() {
        if (wifiStarted) return;
        wifiStarted = true;
        constexpr int port = 8765;
        SetTransferEngines(package + L"\\Engines");
        if (!StartTransferServer(dataRoot, pairing, port)) {
            wifiAddressText.Text(L"Wi-Fi isn't available");
            wifiCodeText.Text(L"—");
            wifiStatusText.Text(L"The transfer couldn't start on this network.");
            return;
        }
#ifndef XSTATION_PUBLIC
        if (Exists(dataRoot + L"\\dev-open-add-page.txt")) {
            std::ofstream dev(dataRoot + L"\\dev-pairing.txt", std::ios::trunc);
            dev << pairing;
        }
#endif
        std::wstring address;
        try {
            for (auto const& host : Windows::Networking::Connectivity::NetworkInformation::GetHostNames()) {
                if (host.Type() != Windows::Networking::HostNameType::Ipv4) continue;
                const std::wstring name = host.CanonicalName().c_str();
                // A home network address first: VPN adapters (such as 26.x) can't be reached by other devices.
                int a = 0, b = 0;
                swscanf_s(name.c_str(), L"%d.%d", &a, &b);
                const bool home = (a == 192 && b == 168) || a == 10 || (a == 172 && b >= 16 && b <= 31);
                if (address.empty() || home) address = name;
                if (home) break;
            }
        } catch (...) {}
        wifiAddress = address.empty() ? L"this Xbox's IP address" : address;
        wifiAddressText.Text(address.empty() ? L"this Xbox's IP address" : wifiAddress + L":" + std::to_wstring(port));
        wifiCodeText.Text(Widen(pairing));
    }

    std::wstring UsbLabel(const UsbItem& item) {
        if (item.pkg) {
            if (!item.updatePath.empty())
                return L"PKG:  " + item.title + L"   (" + item.id + L")   game + update " + VersionText(item.updateVersion) + L"   " +
                       FormatBytes(item.bytes + item.updateBytes) + L"   A converts both";
            const std::wstring kind = item.category == "gp" ? L"update" : item.category == "gd" ? L"game" : L"PKG";
            return L"PKG:  " + item.title + (item.id.empty() ? L"" : L"   (" + item.id + L")") + L"   " + kind +
                   (item.version.empty() ? L"" : L" " + item.version) + L"   " + FormatBytes(item.bytes) + L"   A converts it";
        }
        bool mounted = false;
        for (const auto& m : ReadMounts()) if (m.id == item.id && m.folder == item.path) mounted = true;
        return item.title + L"   (" + item.id + L")   " + (item.bytes ? FormatBytes(item.bytes) + L"   " : L"") +
               (!item.prepared ? (GamePrepareSupported(GameEngineId(item.id)) ? L"not prepared   A prepares it" : L"not prepared   A checks it") : mounted ? L"MOUNTED" : L"");
    }

    // What A and Y do for an item on the drive (empty: nothing).
    std::wstring UsbAction(const UsbItem& item) const {
        if (item.pkg) return item.id.empty() ? L"" : L"Convert";
        if (!item.prepared) return GamePrepareSupported(GameEngineId(item.id)) ? L"Prepare" : L"Check";
        return L"Copy to Xbox";
    }
    std::wstring UsbYAction(const UsbItem& item, const std::vector<Mount>& mounts) const {
        if (item.pkg || !item.prepared) return L"";
        for (const auto& m : mounts) if (m.id == item.id && m.folder == item.path) return L"Unmount";
        return L"Mount";
    }

    Border StatusTag(const std::wstring& label, Color color) {
        Border b;
        b.Background(SolidColorBrush(color));
        b.CornerRadius(CornerRadius{4, 4, 4, 4});
        b.Padding(Thickness{7, 1, 7, 2});
        b.VerticalAlignment(VerticalAlignment::Center);
        b.Child(Text(label, 11));
        return b;
    }

    FrameworkElement UsbRow(const UsbItem& item, bool on, const std::vector<Mount>& mounts) {
        Border row;
        row.CornerRadius(CornerRadius{10, 10, 10, 10});
        row.Padding(Thickness{10, 8, 12, 8});
        row.BorderThickness(Thickness{2, 2, 2, 2});
        row.BorderBrush(on ? White() : White(0));
        row.Background(on ? White(20) : White(0));
        Grid g;
        ColumnDefinition c0, c1, c2;
        c0.Width(GridLengthHelper::FromPixels(66));
        c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        c2.Width(GridLengthHelper::Auto());
        g.ColumnDefinitions().Append(c0);
        g.ColumnDefinitions().Append(c1);
        g.ColumnDefinitions().Append(c2);
        Border art;
        art.Width(52);
        art.Height(52);
        art.CornerRadius(CornerRadius{8, 8, 8, 8});
        art.Background(Solid(255, 40, 52, 72));
        if (item.art) {
            ImageBrush brush;
            brush.ImageSource(item.art);
            brush.Stretch(Stretch::UniformToFill);
            art.Background(brush);
        } else {
            auto glyph = Text(item.pkg ? L"PKG" : item.id.substr(0, 4), 11, White(170));
            glyph.HorizontalAlignment(HorizontalAlignment::Center);
            glyph.VerticalAlignment(VerticalAlignment::Center);
            art.Child(glyph);
        }
        g.Children().Append(art);
        bool mounted = false;
        for (const auto& m : mounts) if (m.id == item.id && m.folder == item.path) mounted = true;
        std::wstring chip, sub;
        Color chipColor{255, 95, 94, 90};
        if (item.pkg) {
            chip = item.category == "gp" ? L"Update " + item.version : item.category == "gd" ? L"Game " + item.version : L"PKG";
            chipColor = item.category.empty() ? Color{255, 95, 94, 90} : Color{255, 59, 109, 17};
            sub = L"PKG \u00B7 " + FormatBytes(item.bytes) + (item.category == "gp" ? L" \u00B7 with the game PKG on this drive" : L"");
            const std::wstring needed = item.id.empty() ? L"" : GameNeededUpdate(item.id);
            if (!item.updatePath.empty()) {
                chip = L"Game + update " + VersionText(item.updateVersion);
                sub = L"2 PKGs \u00B7 " + FormatBytes(item.bytes + item.updateBytes) +
                      (item.updateVersion == needed ? L" \u00B7 A converts both" : L" \u00B7 needs update " + VersionText(needed) + L", not this one");
            } else if (item.category == "gd" && !needed.empty() && item.version != needed) {
                sub += L" \u00B7 needs its update " + VersionText(needed) + L" PKG on this drive too";
            } else if (item.category == "gp" && needed.empty() && GamePrepareSupported(GameEngineId(item.id))) {
                sub = L"PKG \u00B7 " + FormatBytes(item.bytes) + L" \u00B7 not needed: XStation plays this game with no update";
            }
            // 2026-11-18: the check from the game files (StartCompatChecks), shown before anything is converted.
            if (item.category == "gd") {
                if (item.compat == 1) {
                    chipColor = Color{255, 15, 110, 86};
                    sub = (item.updatePath.empty() ? L"PKG \u00B7 " + FormatBytes(item.bytes) : L"2 PKGs \u00B7 " + FormatBytes(item.bytes + item.updateBytes)) +
                          L" \u00B7 Compatible" + (item.compatNote.empty() ? L"" : L" (" + item.compatNote + L")") +
                          (item.updatePath.empty() ? L" \u00B7 A converts it" : L" \u00B7 A converts both");
                } else if (item.compat == 0) {
                    chip = L"Not compatible";
                    chipColor = Color{255, 150, 40, 40};
                    sub = L"PKG \u00B7 " + FormatBytes(item.bytes + item.updateBytes) + L" \u00B7 not a version or region XStation can play";
                } else if (item.compat == -1) {
                    chip = L"Can't check";
                    chipColor = Color{255, 133, 79, 11};
                    sub = L"PKG \u00B7 " + FormatBytes(item.bytes + item.updateBytes) + L" \u00B7 " + item.compatNote;
                } else if (needed.empty() || !item.updatePath.empty() || item.version == needed) {
                    sub += L" \u00B7 checking if it's compatible\u2026";
                }
            }
            if (item.id.empty()) sub = L"PKG \u00B7 " + FormatBytes(item.bytes) + L" \u00B7 XStation can't read it";
        } else if (!item.prepared) {
            chip = L"Not prepared";
            chipColor = Color{255, 133, 79, 11};
            sub = L"Game folder" + (item.bytes ? L" \u00B7 " + FormatBytes(item.bytes) : L"") + (GamePrepareSupported(GameEngineId(item.id)) ? L"" : L" \u00B7 another region? A checks it");
        } else if (mounted) {
            chip = L"Mounted";
            chipColor = Color{255, 24, 95, 165};
            sub = L"Game folder" + (item.bytes ? L" \u00B7 " + FormatBytes(item.bytes) : L"") + L" \u00B7 plays from this drive";
        } else {
            chip = L"Ready";
            chipColor = Color{255, 15, 110, 86};
            sub = L"Game folder" + (item.bytes ? L" \u00B7 " + FormatBytes(item.bytes) : L"") + L" \u00B7 mount it or copy it";
        }
        StackPanel mid;
        mid.Spacing(3);
        mid.VerticalAlignment(VerticalAlignment::Center);
        mid.Margin(Thickness{2, 0, 14, 0});
        StackPanel titleRow;
        titleRow.Orientation(Orientation::Horizontal);
        titleRow.Spacing(8);
        auto name = Text(item.title.empty() ? item.name : item.title, 17);
        name.MaxWidth(420);
        titleRow.Children().Append(name);
        titleRow.Children().Append(StatusTag(chip, chipColor));
        mid.Children().Append(titleRow);
        mid.Children().Append(Text(sub, 13, White(160)));
        Grid::SetColumn(mid, 1);
        g.Children().Append(mid);
        const std::wstring action = UsbAction(item);
        if (!action.empty()) {
            FrameworkElement right{nullptr};
            if (on) {
                Border button;
                button.Background(White());
                button.CornerRadius(CornerRadius{6, 6, 6, 6});
                button.Padding(Thickness{16, 7, 16, 7});
                auto label = Text(action, 14, Solid(255, 10, 16, 28));
                label.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                button.Child(label);
                right = button;
            } else {
                right = Text(action, 13, White(140));
            }
            right.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetColumn(right, 2);
            g.Children().Append(right);
        }
        row.Child(g);
        return row;
    }

    void RefreshAddHints(const std::vector<Mount>& mounts) {
        hintsAdd.Children().Clear();
        hintsAdd.Children().Append(Hint(L"X", L"RESCAN", Color{255, 40, 90, 170}));
        if (usbSelected < usb.size()) {
            std::wstring y = UsbYAction(usb[usbSelected], mounts), a = UsbAction(usb[usbSelected]);
            std::transform(y.begin(), y.end(), y.begin(), [](wchar_t c) { return (wchar_t)towupper(c); });
            std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return (wchar_t)towupper(c); });
            if (!y.empty()) hintsAdd.Children().Append(Hint(L"Y", y, Color{255, 196, 150, 20}));
            if (!a.empty()) hintsAdd.Children().Append(Hint(L"A", a, Color{255, 40, 140, 70}));
        }
        hintsAdd.Children().Append(Hint(L"B", L"BACK", Color{255, 170, 44, 44}));
    }

    void RefreshUsb() {
        usbList.Children().Clear();
        usbHeader.Text(usbDriveLine.empty() ? L"USB drive" : usbDriveLine);
        const auto mounts = ReadMounts();
        auto message = [&](const std::wstring& text) {
            auto t = Text(text, 14, White(200));
            t.TextWrapping(TextWrapping::WrapWholeWords);
            t.TextTrimming(TextTrimming::None);
            t.Margin(Thickness{2, 4, 0, 4});
            usbList.Children().Append(t);
        };
        if (!usbStatus.empty()) message(usbStatus);
        else if (usb.empty() && !usbScanning) message(L"No games on a USB drive. Put PKGs at the top of the drive, or game folders in a folder named XStation, then press X.");
        const size_t first = usbSelected >= 5 ? usbSelected - 4 : 0;
        for (size_t i = first; i < usb.size() && i < first + 5; ++i) usbList.Children().Append(UsbRow(usb[i], i == usbSelected, mounts));
        RefreshAddHints(mounts);
    }

    // USB access check (2026-11-09): which calls reach a USB drive from this app. Decides how "Mount" can work.
    static std::wstring ProbeError(DWORD e) { return L"error " + std::to_wstring(e); }
    void ProbeList(std::wofstream& out, const std::wstring& dir, bool fromApp, int depth, int& shown, std::vector<std::wstring>& files) {
        WIN32_FIND_DATAW f{};
        const std::wstring pattern = dir + L"\\*";
        HANDLE h = fromApp ? FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0)
                           : FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) { out << L"    list " << dir << L": " << ProbeError(GetLastError()) << L"\n"; return; }
        std::vector<std::wstring> children;
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            const std::wstring path = dir + L"\\" + f.cFileName;
            const bool folder = (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (shown < 60) { out << L"    " << (folder ? L"[dir] " : L"      ") << path << L"\n"; ++shown; }
            if (folder) children.push_back(path);
            else if (files.size() < 400) files.push_back(path);
        } while (FindNextFileW(h, &f));
        FindClose(h);
        if (depth < 5) for (const auto& c : children) ProbeList(out, c, fromApp, depth + 1, shown, files);
    }
    void ProbeOpen(std::wofstream& out, const std::wstring& path) {
        for (int fromApp = 0; fromApp < 2; ++fromApp) {
            HANDLE h = fromApp ? CreateFileFromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)
                               : CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
            out << L"    " << (fromApp ? L"CreateFileFromAppW " : L"CreateFile2        ") << path << L": ";
            if (h == INVALID_HANDLE_VALUE) { out << ProbeError(GetLastError()) << L"\n"; continue; }
            char buffer[4096];
            DWORD got = 0;
            LARGE_INTEGER size{};
            GetFileSizeEx(h, &size);
            const BOOL first = ReadFile(h, buffer, sizeof(buffer), &got, nullptr);
            out << L"opened, size " << size.QuadPart << L", read " << (first ? std::to_wstring(got) : ProbeError(GetLastError()));
            if (size.QuadPart > 8192) { // positioned read, as the engines do
                OVERLAPPED at{};
                at.Offset = DWORD((size.QuadPart / 2) & ~4095ull);
                at.OffsetHigh = DWORD((size.QuadPart / 2) >> 32);
                got = 0;
                const BOOL mid = ReadFile(h, buffer, sizeof(buffer), &got, &at);
                out << L", read at middle " << (mid ? std::to_wstring(got) : ProbeError(GetLastError()));
            }
            out << L"\n";
            CloseHandle(h);
        }
    }
    void ProbeRoot(std::wofstream& out, const std::wstring& root) {
        out << L"  root " << root << L"\n";
        const DWORD a = GetFileAttributesW(root.c_str());
        out << L"    GetFileAttributesW: " << (a == INVALID_FILE_ATTRIBUTES ? ProbeError(GetLastError()) : std::to_wstring(a)) << L"\n";
        WIN32_FILE_ATTRIBUTE_DATA ad{};
        out << L"    GetFileAttributesExFromAppW: " << (GetFileAttributesExFromAppW(root.c_str(), GetFileExInfoStandard, &ad) ? std::to_wstring(ad.dwFileAttributes) : ProbeError(GetLastError())) << L"\n";
        wchar_t fs[32]{}, label[64]{};
        if (GetVolumeInformationW(root.c_str(), label, 64, nullptr, nullptr, nullptr, fs, 32)) out << L"    volume '" << label << L"', file system " << fs << L"\n";
        else out << L"    GetVolumeInformationW: " << ProbeError(GetLastError()) << L"\n";
        ULARGE_INTEGER freeBytes{}, total{};
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, &total, nullptr)) out << L"    free " << freeBytes.QuadPart / 1048576 << L" MiB of " << total.QuadPart / 1048576 << L" MiB\n";
        else out << L"    GetDiskFreeSpaceExW: " << ProbeError(GetLastError()) << L"\n";
        std::wstring base = root;
        while (!base.empty() && base.back() == L'\\') base.pop_back();
        for (int fromApp = 0; fromApp < 2; ++fromApp) {
            out << L"   " << (fromApp ? L" listing with FindFirstFileExFromAppW:" : L" listing with FindFirstFileExW:") << L"\n";
            int shown = 0;
            std::vector<std::wstring> files;
            ProbeList(out, base, fromApp != 0, 0, shown, files);
            out << L"    " << files.size() << L" files found\n";
            // open one file of each kind: no extension, and the first few extensions
            std::vector<std::wstring> kinds;
            int opened = 0;
            for (const auto& file : files) {
                const size_t slash = file.find_last_of(L'\\'), dot = file.find_last_of(L'.');
                const std::wstring kind = dot == std::wstring::npos || dot < slash ? L"(none)" : file.substr(dot);
                if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end()) continue;
                kinds.push_back(kind);
                ProbeOpen(out, file);
                if (++opened >= 8) break;
            }
        }
    }
    Windows::Foundation::IAsyncAction ProbeUsb(Windows::Foundation::Collections::IVectorView<StorageFolder> drives) {
        auto keep = get_strong();
        std::wofstream out(dataRoot + L"\\xstation-usb-probe.txt", std::ios::trunc);
        out << L"XStation USB check, " << drives.Size() << L" removable folder(s)\n";
        for (auto const& drive : drives) {
            const std::wstring path = drive.Path().c_str();
            out << L"drive '" << drive.DisplayName().c_str() << L"' path '" << path << L"'\n";
            try {
                auto items = co_await drive.GetItemsAsync();
                out << L"  storage API sees " << items.Size() << L" item(s) at the top:\n";
                for (auto const& item : items) out << L"    " << (item.IsOfType(StorageItemTypes::Folder) ? L"[dir] " : L"      ") << item.Name().c_str() << L"\n";
                auto holder = co_await drive.TryGetItemAsync(L"XStation");
                if (holder && holder.IsOfType(StorageItemTypes::Folder)) {
                    for (auto const& game : co_await holder.as<StorageFolder>().GetFoldersAsync()) {
                        auto inner = co_await game.GetItemsAsync();
                        out << L"  storage API, XStation\\" << game.Name().c_str() << L": " << inner.Size() << L" item(s)\n";
                        for (auto const& item : inner) out << L"    " << item.Name().c_str() << L"\n";
                    }
                }
            } catch (hresult_error const& e) {
                out << L"  storage API failed: " << e.message().c_str() << L"\n";
            }
            if (!path.empty()) ProbeRoot(out, path);
            out.flush();
        }
        for (wchar_t letter = L'E'; letter <= L'K'; ++letter) {
            const std::wstring root = std::wstring(1, letter) + L":\\";
            const DWORD a = GetFileAttributesW(root.c_str());
            out << L"letter " << root << L": " << (a == INVALID_FILE_ATTRIBUTES ? ProbeError(GetLastError()) : L"attributes " + std::to_wstring(a)) << L"\n";
        }
        out << L"done\n";
    }

    bool waking = false;
    bool MountMissing() const {
        for (const auto& g : games) if (g.mounted && g.usbMissing) return true;
        return false;
    }
    // An idle USB drive fails the first file check after it wakes; the storage API's drive list wakes it.
    fire_and_forget WakeUsb() {
        if (waking) co_return;
        auto keep = get_strong();
        waking = true;
        try { co_await KnownFolders::RemovableDevices().GetFoldersAsync(); } catch (...) {}
        waking = false;
        if (MountMissing() && !launching && !deleting) { Rescan(); Refresh(false); }
    }

    bool CopyRunning() const { return copyState == 1 || copyState == 2; }
    std::wstring CopyId() { std::lock_guard<std::mutex> lock(copyLock); return copyId; }
    void UsbLog(const std::wstring& line) {
        std::ofstream log(dataRoot + L"\\xstation-usb.log", std::ios::app | std::ios::binary);
        log << GetTickCount64() << ' ' << ToUtf8(line) << '\n';
    }

    fire_and_forget ScanUsb() {
        if (usbScanning || CopyRunning()) co_return;
        auto keep = get_strong();
        usbScanning = true;
        usbScannedOnce = true;
        usb.clear();
        usbSelected = 0;
        usbStatus = L"Looking for USB drives\u2026";
        RefreshUsb();
        try {
            auto drives = co_await KnownFolders::RemovableDevices().GetFoldersAsync();
            usbDriveLine.clear();
            usbFree.clear();
            for (auto const& drive : drives) {
                std::wstring line = L"USB drive · " + std::wstring(drive.DisplayName().c_str());
                try {
                    auto props = co_await drive.Properties().RetrievePropertiesAsync(std::vector<hstring>{L"System.FreeSpace"});
                    if (props.HasKey(L"System.FreeSpace")) {
                        const uint64_t freeBytes = unbox_value<uint64_t>(props.Lookup(L"System.FreeSpace"));
                        line += L" · " + FormatBytes(freeBytes) + L" free";
                        std::wstring driveRoot = drive.Path().c_str();
                        while (!driveRoot.empty() && driveRoot.back() == L'\\') driveRoot.pop_back();
                        usbFree[driveRoot] = freeBytes;
                    }
                } catch (...) {}
                usbDriveLine = usbDriveLine.empty() ? line : usbDriveLine + L"   " + line.substr(12);
            }
            if (usbProbeWanted) { usbProbeWanted = false; try { co_await ProbeUsb(drives); } catch (...) {} }
            for (auto const& drive : drives) {
                std::wstring root = drive.Path().c_str();
                while (!root.empty() && root.back() == L'\\') root.pop_back();
                if (root.empty()) continue;
                WIN32_FIND_DATAW f;
                HANDLE h = FindFirstFileExW((root + L"\\*.pkg").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        UsbItem item;
                        item.pkg = true;
                        item.name = f.cFileName;
                        item.path = root + L"\\" + f.cFileName;
                        item.bytes = (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
                        PkgParams params;
                        std::wstring perr;
                        if (PkgReadParams(item.path, params, perr)) {
                            item.title = FromUtf8(params.title);
                            item.id = FromUtf8(params.titleId);
                            item.category = params.category;
                            item.version = FromUtf8(params.appVer);
                            if (!params.icon.empty()) {
                                item.art = BitmapImage();
                                item.art.DecodePixelWidth(104);
                                LoadBytes(item.art, std::move(params.icon));
                            }
                        } else {
                            item.title = item.name;
                            UsbLog(L"pkg " + item.name + L": " + perr);
                        }
                        usb.push_back(item);
                    } while (FindNextFileW(h, &f));
                    FindClose(h);
                }
                const std::wstring holder = root + L"\\XStation";
                for (const auto& id : Folders(holder)) {
                    const std::wstring folder = holder + L"\\" + id;
                    const bool update109 = Exists(folder + L"\\GameData109\\sce_sys\\param.sfo");
                    const std::wstring sfo = folder + (update109 ? L"\\GameData109\\sce_sys\\param.sfo" : L"\\GameData\\sce_sys\\param.sfo");
                    if (!Exists(sfo)) continue;
                    UsbItem item;
                    item.id = id;
                    item.name = id;
                    item.path = folder;
                    auto values = ReadSfo(sfo);
                    item.title = values.count("TITLE") ? Widen(values["TITLE"]) : id;
                    item.prepared = Exists(folder + (update109 ? L"\\GameBoot109\\boot-linked.bin" : L"\\GameBoot\\boot-linked.bin"));
                    std::ifstream info(folder + L"\\xstation-game.txt");
                    if (info) info >> item.bytes;
                    const std::wstring icon = folder + (update109 ? L"\\GameData109\\sce_sys\\icon0.png" : L"\\GameData\\sce_sys\\icon0.png");
                    if (Exists(icon)) {
                        item.art = BitmapImage();
                        item.art.DecodePixelWidth(104);
                        LoadFile(item.art, icon);
                    }
                    usb.push_back(item);
                }
            }
            for (size_t g = 0; g < usb.size(); ++g) {
                if (!usb[g].pkg || usb[g].category != "gd" || usb[g].id.empty()) continue;
                const std::wstring needed = GameNeededUpdate(usb[g].id);
                if (needed.empty()) continue;
                size_t best = usb.size();
                for (size_t i = 0; i < usb.size(); ++i) {
                    const UsbItem& u = usb[i];
                    if (!u.pkg || u.category != "gp" || u.id != usb[g].id || u.paired) continue;
                    if (best == usb.size()) { best = i; continue; }
                    const bool bestExact = usb[best].version == needed;
                    if (!bestExact && (u.version == needed || u.version > usb[best].version)) best = i;
                }
                if (best == usb.size()) continue;
                usb[g].updatePath = usb[best].path;
                usb[g].updateVersion = usb[best].version;
                usb[g].updateBytes = usb[best].bytes;
                usb[best].paired = true;
            }
            usb.erase(std::remove_if(usb.begin(), usb.end(), [](const UsbItem& u) { return u.paired; }), usb.end());
            for (auto& u : usb) {
                if (auto c = compatCache.find(CompatKey(u)); c != compatCache.end()) { u.compat = c->second.first; u.compatNote = c->second.second; }
            }
            StartCompatChecks();
            if (usbSelected >= usb.size()) usbSelected = usb.empty() ? 0 : usb.size() - 1;
            usbStatus = drives.Size() == 0 ? L"No USB drive found. Plug one in, then press X." : L"";
        } catch (hresult_error const& e) {
            usbStatus = L"USB access failed: " + std::wstring(e.message().c_str());
        }
        usbScanning = false;
        RefreshUsb();
        if (!devUsbAction.empty()) { // dev switch, tests without a controller
            const std::wstring action = devUsbAction;
            devUsbAction.clear();
            const size_t space = action.find(L' ');
            const std::wstring verb = action.substr(0, space), what = space == std::wstring::npos ? L"" : action.substr(space + 1);
            for (size_t i = 0; i < usb.size(); ++i) {
                if (usb[i].id != what && usb[i].name != what) continue;
                usbSelected = i;
                UsbLog(L"dev action " + action);
                if (verb == L"copy" || verb == L"convert") CopyUsbSelected();
                else if (verb == L"prepare" && !CopyRunning()) PrepareUsbSelected(); // even when it has a GameBoot (same bytes)
                else if (verb == L"mount") MountSelected();
                break;
            }
        }
    }

    // Y: a prepared game folder on the drive goes into the library and plays from there (mounts.txt); Y again unmounts.
    void MountSelected() {
        if (usbScanning || usbSelected >= usb.size()) return;
        const UsbItem item = usb[usbSelected];
        if (item.pkg) { usbStatus = L"A PKG can't be mounted. Press A to convert it into a game folder on the drive, then mount that."; RefreshUsb(); return; }
        auto mounts = ReadMounts();
        const auto it = std::find_if(mounts.begin(), mounts.end(), [&](const Mount& m) { return m.id == item.id; });
        if (it != mounts.end() && it->folder == item.path) {
            mounts.erase(it);
            if (!WriteMounts(mounts)) { usbStatus = L"Couldn't save the change."; RefreshUsb(); return; }
            usbStatus = L"Unmounted " + item.title + L". It's still on the USB drive.";
            UsbLog(L"unmount " + item.id);
        } else {
            if (!item.prepared) { usbStatus = item.title + L" isn't prepared yet. Press A to prepare it."; RefreshUsb(); return; }
            for (const auto& g : games) {
                if (g.id == item.id && !g.mounted) { usbStatus = item.title + L" is already on this Xbox. Play that copy, or delete it first to play from the USB drive."; RefreshUsb(); return; }
            }
            if (it != mounts.end()) mounts.erase(it); // the same game from another drive or folder
            mounts.push_back({item.id, item.path, item.title});
            if (!WriteMounts(mounts)) { usbStatus = L"Couldn't save the change."; RefreshUsb(); return; }
            usbStatus = L"Mounted " + item.title + L". It's in your library and plays straight from the USB drive. Keep the drive plugged in while you play.";
            UsbLog(L"mount " + item.id + L" " + item.path);
        }
        Rescan();
        RefreshUsb();
    }

    // A: copy to the Xbox on a worker thread. Files already there with the same size are kept, so an interrupted copy
    // picks up where it stopped. The game's sce_sys comes first, so its tile appears with its art and a bar.
    static void ListTree(const std::wstring& dir, const std::wstring& rel, std::vector<std::pair<std::wstring, uint64_t>>& out, bool& ok) {
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) { ok = false; return; }
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            const std::wstring name = rel.empty() ? std::wstring(f.cFileName) : rel + L"\\" + f.cFileName;
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ListTree(dir + L"\\" + f.cFileName, name, out, ok);
            } else if (_wcsicmp(f.cFileName, L".xstation-copying") && _wcsicmp(f.cFileName, L"dzn_icd.x86_64.json")) {
                out.push_back({name, (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow});
            }
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }

    void CopyWorker(std::wstring source, std::wstring target, bool game) {
        auto fail = [&](const std::wstring& why) {
            { std::lock_guard<std::mutex> lock(copyLock); copyError = why; }
            UsbLog(L"copy failed: " + why);
            copyState = 4;
        };
        std::vector<std::pair<std::wstring, uint64_t>> files;
        bool listed = true;
        if (game) ListTree(source, L"", files, listed);
        else {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &a)) listed = false;
            else files.push_back({L"", (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow});
        }
        if (!listed || files.empty()) { fail(L"Couldn't read the USB drive. Is it still plugged in?"); return; }
        std::stable_partition(files.begin(), files.end(), [](const auto& f) {
            return f.first.find(L"\\sce_sys\\") != std::wstring::npos || f.first == L"xstation-game.txt";
        });
        uint64_t total = 0, present = 0;
        for (const auto& [rel, size] : files) {
            total += size;
            WIN32_FILE_ATTRIBUTE_DATA a{};
            const std::wstring dst = rel.empty() ? target : target + L"\\" + rel;
            if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &a) && ((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow) == size) present += size;
        }
        copyTotal = total;
        copyFilesTotal = files.size();
        ULARGE_INTEGER freeBytes{};
        if (GetDiskFreeSpaceExW(dataRoot.c_str(), &freeBytes, nullptr, nullptr) && total - present + (512ull << 20) > freeBytes.QuadPart) {
            fail(NotEnoughStorageText(total - present + (512ull << 20), freeBytes.QuadPart, L"the Xbox")); // 2026-11-18
            return;
        }
        UsbLog(L"copy " + source + L" -> " + target + L": " + std::to_wstring(files.size()) + L" files, " + std::to_wstring(total) + L" bytes, " + std::to_wstring(present) + L" already there");
        const auto began = std::chrono::steady_clock::now();
        if (game) {
            CreateDirectoryW(target.c_str(), nullptr);
            std::ofstream marker(target + L"\\.xstation-copying", std::ios::trunc | std::ios::binary);
            marker << ToUtf8(source) << '\n';
        }
        copyState = 2;
        std::vector<char> buffer(8u << 20);
        bool artSignalled = false;
        for (const auto& [rel, size] : files) {
            const std::wstring src = rel.empty() ? source : source + L"\\" + rel, dst = rel.empty() ? target : target + L"\\" + rel;
            if (!artSignalled && rel.find(L"\\sce_sys\\") == std::wstring::npos && rel != L"xstation-game.txt") { artSignalled = true; copyArtReady = true; }
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &a) && ((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow) == size) {
                copyDone += size;
                ++copyFiles;
                continue;
            }
            for (size_t at = target.size() + 1; at < dst.size() && (at = dst.find(L'\\', at)) != std::wstring::npos; ++at) CreateDirectoryW(dst.substr(0, at).c_str(), nullptr);
            CREATEFILE2_EXTENDED_PARAMETERS sequential{sizeof(sequential)};
            sequential.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            sequential.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
            HANDLE in = CreateFile2(src.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &sequential);
            if (in == INVALID_HANDLE_VALUE) { fail(L"Couldn't open " + (rel.empty() ? source : rel) + L" on the USB drive (error " + std::to_wstring(GetLastError()) + L")."); return; }
            const std::wstring part = dst + L".part";
            HANDLE out = CreateFile2(part.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
            if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); fail(L"Couldn't write to the Xbox (error " + std::to_wstring(GetLastError()) + L")."); return; }
            bool ok = true;
            uint64_t copied = 0;
            for (;;) {
                DWORD got = 0, wrote = 0;
                if (!ReadFile(in, buffer.data(), (DWORD)buffer.size(), &got, nullptr)) { ok = false; break; }
                if (!got) break;
                if (!WriteFile(out, buffer.data(), got, &wrote, nullptr) || wrote != got) { ok = false; break; }
                copied += got;
                copyDone += got;
            }
            CloseHandle(in);
            CloseHandle(out);
            if (!ok || copied != size || !MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileW(part.c_str());
                fail(copied != size && ok ? L"A file changed size while copying: " + rel : L"Copy stopped at " + (rel.empty() ? source : rel) + L". The USB drive may have been unplugged, or the Xbox is out of space.");
                return;
            }
            ++copyFiles;
        }
        if (game) DeleteFileW((target + L"\\.xstation-copying").c_str());
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        UsbLog(L"copy done: " + std::to_wstring(total - present) + L" bytes copied in " + std::to_wstring((int)seconds) + L" s (" +
               std::to_wstring(seconds > 0 ? (int)((total - present) / 1048576.0 / seconds) : 0) + L" MB/s)");
        copyArtReady = true;
        copyState = 3;
    }

    // A on "Use up the PKG?": convert the same PKG again, this time using it up.
    void ConfirmUseUp() {
        if (!confirmOpen || confirmKind != 3) return;
        confirmOpen = false;
        confirmKind = 0;
        if (usbScanning || CopyRunning()) { Refresh(false); return; }
        size_t found = usb.size();
        for (size_t i = 0; i < usb.size(); ++i) if (usb[i].pkg && usb[i].path == shrinkOfferPath) found = i;
        if (found == usb.size()) { usbStatus = L"The PKG isn't on the drive any more."; Refresh(false); RefreshUsb(); return; }
        usbSelected = found;
        UsbLog(L"use-up: yes for " + shrinkOfferPath);
        Refresh(false);
        preparing = false;
        ConvertUsbSelected(true);
    }

    // A on the question after a checked conversion: delete the PKG(s) it used (FromApp: they're on the USB drive).
    void DeleteConvertedPkgs() {
        if (!confirmOpen || confirmKind != 2) return;
        confirmOpen = false;
        confirmKind = 0;
        size_t deleted = 0;
        uint64_t freed = 0;
        std::wstring failed;
        for (const auto& p : convertedPkgs) {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            const uint64_t size = GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) ? (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow : 0;
            if (DeleteFileFromAppW(p.c_str())) { ++deleted; freed += size; UsbLog(L"deleted pkg " + p); }
            else { failed = p.substr(p.find_last_of(L'\\') + 1); UsbLog(L"couldn't delete pkg " + p + L": error " + std::to_wstring(GetLastError())); }
        }
        convertedPkgs.clear();
        usbStatus = failed.empty() ? L"Deleted the PKG" + std::wstring(deleted > 1 ? L"s" : L"") + L", " + FormatBytes(freed) + L" freed."
                                   : L"Couldn't delete " + failed + L".";
        Refresh(false);
        ScanUsb();
    }

    // A on a PKG: convert it into <drive>\XStation\<id>\GameData on the same drive (pkg_extract.cpp). An update is
    // converted with the game PKG of the same title from the drive; the game PKG on its own converts the game alone.
    bool converting = false;
    std::map<std::wstring, uint64_t> usbFree; // drive root ("E:") -> free bytes, from the last scan
    std::vector<std::wstring> convertedPkgs;  // the PKGs of the last conversion, offered for deletion when it checked out
    std::atomic<bool> convertPrepared{false};
    // Using the PKG up (2026-11-12): the last convert didn't fit but would that way; asked before anything is cut.
    std::atomic<bool> shrinkOffer{false}, convertUsedUp{false};
    std::wstring shrinkOfferPath;   // the PKG the player picked (found again in the USB list on A)
    uint64_t shrinkOfferNeed = 0, shrinkOfferNormal = 0, shrinkOfferFree = 0;
    std::atomic<bool> preparing{false};
    std::wstring prepareResult; // under copyLock
    PkgProgress convertProgress;

    // A on a game folder on the drive without its prepared image: make it on the console.
    void PrepareUsbSelected() {
        const UsbItem item = usb[usbSelected];
        {
            std::lock_guard<std::mutex> lock(copyLock);
            copyError.clear();
            prepareResult.clear();
            copyTitle = item.title;
            copyId.clear();
            copyTarget = item.path;
            copyArt = item.art;
        }
        copyIsGame = false;
        converting = true;
        preparing = true;
        convertProgress.done = convertProgress.total = 0;
        copyState = 2;
        shownCopyState = -1;
        usbStatus = L"";
        RefreshUsb();
        std::thread([this, keep = get_strong(), id = item.id, folder = item.path] {
            std::string detail;
            std::wstring err;
            const auto began = std::chrono::steady_clock::now();
            std::wstring engine;
            const bool ok = GamePrepareAny(id, folder, package + L"\\Engines", engine, err, detail);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            UsbLog(L"prepare " + id + L": " + (ok ? L"done" : err) + L" in " + std::to_wstring((int)s) + L" s\n" + FromUtf8(detail));
            {
                std::lock_guard<std::mutex> lock(copyLock);
                if (ok) prepareResult = L"It's prepared: mount it (Y) or copy it to the Xbox (A).";
                else copyError = err;
            }
            copyState = ok ? 3 : 4;
        }).detach();
    }
    // 2026-11-18: game PKGs on the drive (with their update when paired) are checked one by one in the background right after
    // a scan; each row shows the result, and A converts only a compatible one. Results are kept per file and size.
    // An update on its own, or a game whose engine needs an update that isn't on the drive, isn't checked (its row says why).
    void StartCompatChecks() {
        if (compatRunning) return;
        struct Job { std::wstring key, id; std::vector<std::wstring> pkgs; };
        std::vector<Job> jobs;
        for (const auto& u : usb) {
            if (!u.pkg || u.id.empty() || u.category != "gd" || u.compat != -2) continue;
            if (!GameNeededUpdate(u.id).empty() && u.updatePath.empty() && u.version != GameNeededUpdate(u.id)) continue;
            Job j{CompatKey(u), u.id, {u.path}};
            if (!u.updatePath.empty()) j.pkgs.push_back(u.updatePath);
            jobs.push_back(j);
        }
        if (jobs.empty()) return;
        compatRunning = true;
        const auto dispatcher = Window::Current().Dispatcher();
        std::thread([this, keep = get_strong(), dispatcher, jobs] {
            for (const auto& j : jobs) {
                std::wstring engine, error;
                std::string detail;
                const auto began = std::chrono::steady_clock::now();
                const int result = CheckPkgsCompatible(j.pkgs, j.id, local + L"\\compat-check", package + L"\\Engines", engine, error, detail);
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                std::wstring note;
                if (result == 1) {
                    note = engine;
                    for (const auto& c : kCompatible) if (engine == c.id) note = c.name;
                } else if (result == -1) {
                    note = error;
                }
                UsbLog(L"compat check " + j.id + L": " + (result == 1 ? L"compatible (" + engine + L")" : result == 0 ? std::wstring(L"not compatible") : L"couldn't check: " + error) +
                       L" in " + std::to_wstring((int)s) + L" s\n" + FromUtf8(detail));
                dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [this, keep, key = j.key, result, note] {
                    compatCache[key] = {result, note};
                    for (auto& u : usb) if (CompatKey(u) == key) { u.compat = result; u.compatNote = note; }
                    if (tab == 1 && !usbScanning) RefreshUsb();
                });
            }
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [this, keep] {
                compatRunning = false;
                StartCompatChecks(); // rows a rescan added meanwhile
            });
        }).detach();
    }

    void ConvertUsbSelected(bool useUp = false) {
        const UsbItem item = usb[usbSelected];
        if (item.id.empty()) { usbStatus = L"XStation can't read this PKG: " + item.name; RefreshUsb(); return; }
        for (const wchar_t c : item.id) if (!iswalnum(c)) { usbStatus = L"This PKG's title ID isn't usable."; RefreshUsb(); return; }
        std::vector<std::wstring> pkgs;
        // 2026-11-18: the engine's exact copy decides which PKGs make a game that plays (GameNeededUpdate).
        const std::wstring needed = GameNeededUpdate(item.id);
        if (item.category == "gd" && !needed.empty() && item.version != needed) { // (a game PKG with the update built in needs none)
            if (item.updatePath.empty()) {
                usbStatus = item.title + L" plays with its update " + VersionText(needed) +
                            L". Put the update PKG on the drive next to the game PKG and press X to look again: then A converts both together.";
                RefreshUsb();
                return;
            }
            if (item.updateVersion != needed) {
                usbStatus = L"The update on the drive is " + VersionText(item.updateVersion) + L", but " + item.title + L" needs update " +
                            VersionText(needed) + L". Put that update's PKG on the drive instead.";
                RefreshUsb();
                return;
            }
        }
        if (item.category == "gp" && needed.empty() && GamePrepareSupported(GameEngineId(item.id))) {
            usbStatus = L"XStation plays " + item.title + L" with no update, so this update isn't needed (it wouldn't play). Convert the game PKG on its own.";
            RefreshUsb();
            return;
        }
        if (item.category == "gp" && !needed.empty() && item.version != needed) {
            usbStatus = L"This update is " + VersionText(item.version) + L", but " + item.title + L" needs update " + VersionText(needed) + L".";
            RefreshUsb();
            return;
        }
        // 2026-11-18: only a game the check found compatible is converted (no time spent on one that can't play).
        if (item.category == "gd") {
            if (item.compat == -2) {
                usbStatus = L"Still checking whether this copy of " + item.title + L" works with XStation. Try again in a moment.";
                RefreshUsb();
                StartCompatChecks();
                return;
            }
            if (item.compat == 0) {
                usbStatus = L"Not compatible: this copy of " + item.title + L" isn't a version or region XStation can play, so it won't be "
                            L"converted. The Compatible tab lists the copies that work.";
                RefreshUsb();
                return;
            }
            if (item.compat == -1) {
                usbStatus = L"XStation couldn't check this PKG, so it won't be converted: " + item.compatNote;
                RefreshUsb();
                return;
            }
        }
        if (!item.updatePath.empty()) pkgs = {item.path, item.updatePath};
        else if (item.category == "gp") {
            for (const auto& other : usb) if (other.pkg && other.id == item.id && other.category == "gd") { pkgs.push_back(other.path); break; }
            if (pkgs.empty()) { usbStatus = L"This is an update. Put the game's own PKG on the drive too, then convert the update again."; RefreshUsb(); return; }
        }
        if (item.updatePath.empty()) pkgs.push_back(item.path);
        const size_t slash = item.path.find(L'\\');
        const std::wstring root = slash == std::wstring::npos ? item.path : item.path.substr(0, slash);
        // Bloodborne's engine uses its 1.09 folder names (GameData109).
        const std::wstring target = root + L"\\XStation\\" + item.id, data = target + L"\\" + GamePrepareDataFolder(GameEngineId(item.id)),
                           marker = target + L"\\.xstation-converting";
        // 2026-11-14: an update for a game already converted on the drive goes on top of it (only the update's files are
        // written), so a game converted without its update doesn't have to be converted again.
        const bool addUpdate = item.category == "gp";
        if (Exists(data) && !Exists(marker) && !addUpdate) {
            usbStatus = L"XStation\\" + item.id + L" is already on the drive. Mount or copy it, or delete that folder to convert again.";
            RefreshUsb();
            return;
        }
        CreateDirectoryFromAppW((root + L"\\XStation").c_str(), nullptr);
        CreateDirectoryFromAppW(target.c_str(), nullptr);
        {
            std::ofstream m(marker, std::ios::trunc | std::ios::binary);
            for (const auto& p : pkgs) m << ToUtf8(p.substr(p.find_last_of(L'\\') + 1)) << '\n';
            if (!m.flush()) { usbStatus = L"Couldn't write to the USB drive."; RefreshUsb(); return; }
        }
        {
            std::lock_guard<std::mutex> lock(copyLock);
            copyError.clear();
            copyTitle = item.title + (!item.updatePath.empty() ? L" + update " + VersionText(item.updateVersion)
                                      : item.category == "gp" ? L" " + item.version : L"");
            copyId.clear();
            copyTarget = target;
            copyArt = item.art;
        }
        copyIsGame = false;
        converting = true;
        convertProgress.done = convertProgress.total = 0;
        convertProgress.files = convertProgress.filesTotal = 0;
        copyArtReady = false;
        copySpeed = 0;
        speedBytes = 0;
        speedAt = copyBegan = std::chrono::steady_clock::now();
        copyState = 1;
        shownCopyState = -1;
        usbStatus = L"";
        RefreshUsb();
        std::wstring list;
        for (const auto& p : pkgs) list += L" " + p;
        const uint64_t freeBytes = usbFree.count(root) ? usbFree[root] : 0;
        UsbLog(L"convert" + list + L" -> " + data + L" (free " + std::to_wstring(freeBytes) + L")");
        convertedPkgs = pkgs;
        convertPrepared = false;
        convertUsedUp = false;
        shrinkOffer = false;
        const std::wstring picked = item.path;
        std::thread([this, keep = get_strong(), pkgs, data, target, marker, freeBytes, useUp, picked] {
            copyState = 2;
            std::wstring err;
            const auto began = std::chrono::steady_clock::now();
            PkgShrink shrink;
            shrink.allowed = useUp;
            const bool converted = PkgExtractGame(pkgs, data, &convertProgress, nullptr, true, err, freeBytes, &shrink, pkgs.size() > 1);
            if (shrink.need)
                UsbLog(L"use-up: allowed " + std::to_wstring(useUp) + L" used " + std::to_wstring(shrink.used) + L" need " + std::to_wstring(shrink.need) +
                       L" normal need " + std::to_wstring(shrink.normalNeed) + L" free " + std::to_wstring(freeBytes));
            if (converted) {
                convertUsedUp = shrink.used;
                DeleteFileFromAppW(marker.c_str());
                {
                    std::ofstream size(target + L"\\xstation-game.txt", std::ios::trunc | std::ios::binary);
                    size << convertProgress.total.load();
                }
                const std::wstring id = target.substr(target.find_last_of(L'\\') + 1);
                {
                    preparing = true;
                    std::string detail;
                    std::wstring perr, engine;
                    const bool prepared = GamePrepareAny(id, target, package + L"\\Engines", engine, perr, detail);
                    UsbLog(L"prepare " + id + L" (engine " + engine + L"): " + (prepared ? L"done" : perr) + L"\n" + FromUtf8(detail));
                    convertPrepared = prepared;
                    std::lock_guard<std::mutex> lock(copyLock);
                    prepareResult = prepared ? L"It's prepared: mount it (Y) or copy it to the Xbox (A)." : L"It couldn't be prepared: " + perr;
                }
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                UsbLog(L"convert done: " + std::to_wstring(convertProgress.files.load()) + L" files, " + std::to_wstring(convertProgress.total.load()) +
                       L" bytes in " + std::to_wstring((int)s) + L" s (" + std::to_wstring(s > 0 ? (int)(convertProgress.total / 1048576.0 / s) : 0) + L" MB/s)");
                copyState = 3;
            } else {
                { std::lock_guard<std::mutex> lock(copyLock); copyError = err; }
                UsbLog(L"convert failed: " + err);
                const bool nothingWritten = !convertProgress.done && !convertProgress.files;
                if (nothingWritten) { // leave the drive as it was
                    DeleteFileFromAppW(marker.c_str());
                    RemoveDirectoryFromAppW(data.c_str());
                    RemoveDirectoryFromAppW(target.c_str());
                }
                // No room for the PKG and the game, but enough when the PKG is used up: ask (FinishCopy).
                if (!useUp && nothingWritten && shrink.need && freeBytes && shrink.normalNeed > freeBytes && shrink.need <= freeBytes) {
                    shrinkOfferPath = picked;
                    shrinkOfferNeed = shrink.need;
                    shrinkOfferNormal = shrink.normalNeed;
                    shrinkOfferFree = freeBytes;
                    shrinkOffer = true;
                }
                copyState = 4;
            }
        }).detach();
    }

    fire_and_forget CopyUsbSelected() {
        if (usbScanning || CopyRunning() || usbSelected >= usb.size()) co_return;
        if (usb[usbSelected].pkg) { preparing = false; ConvertUsbSelected(); co_return; }
        if (!usb[usbSelected].prepared) { PrepareUsbSelected(); co_return; }
        preparing = false;
        converting = false;
        if (gamesDriveMissing) { usbStatus = L"XStation can't write to D:\\DevelopmentFiles, so it can't store games right now."; RefreshUsb(); co_return; }
        const UsbItem item = usb[usbSelected];
        std::wstring target;
        if (item.pkg) target = dataRoot + L"\\Import\\" + item.name;
        else {
            bool safe = !item.id.empty();
            for (const wchar_t c : item.id) if (!iswalnum(c)) safe = false;
            if (!safe) { usbStatus = L"That folder's name isn't a title ID, so it can't be copied."; RefreshUsb(); co_return; }
            target = dataRoot + L"\\Games\\" + item.id;
        }
        {
            std::lock_guard<std::mutex> lock(copyLock);
            copyError.clear();
            copyTitle = item.pkg ? item.name : item.title;
            copyId = item.pkg ? L"" : item.id;
            copyTarget = target;
            copyArt = item.art;
        }
        copyIsGame = !item.pkg;
        copyDone = copyTotal = copyFiles = copyFilesTotal = 0;
        copyArtReady = false;
        copySpeed = 0;
        speedBytes = 0;
        speedAt = copyBegan = std::chrono::steady_clock::now();
        copyState = 1;
        shownCopyState = -1;
        usbStatus = L"";
        RefreshUsb();
        std::thread([this, keep = get_strong(), source = item.path, target, game = !item.pkg] { CopyWorker(source, target, game); }).detach();
    }

    static std::wstring FormatLeft(double seconds) {
        if (seconds < 60) return L"Less than a minute left";
        const int minutes = (int)(seconds / 60 + 0.5);
        if (minutes < 60) return L"About " + std::to_wstring(minutes) + L" min left";
        return L"About " + std::to_wstring(minutes / 60) + L" h " + std::to_wstring(minutes % 60) + L" min left";
    }

    uint64_t Done() const { return converting ? convertProgress.done.load() : copyDone.load(); }
    uint64_t Total() const { return converting ? convertProgress.total.load() : copyTotal.load(); }

    // Poll: the copy's bar and line (Add Game page, the game's tile and its page), every 250 ms.
    void ShowCopy() {
        const int state = copyState;
        if (state == 0) return;
        const auto now = std::chrono::steady_clock::now();
        if (state == 2 && now - speedAt >= std::chrono::seconds(1)) {
            const double dt = std::chrono::duration<double>(now - speedAt).count();
            const double rate = (Done() - speedBytes) / dt;
            copySpeed = copySpeed <= 0 ? rate : copySpeed * 0.7 + rate * 0.3;
            speedBytes = Done();
            speedAt = now;
        }
        std::wstring title, error, id;
        { std::lock_guard<std::mutex> lock(copyLock); title = copyTitle; error = copyError; id = copyId; }
        const uint64_t done = Done(), total = Total();
        const double fraction = total ? double(done) / double(total) : 0.0;
        const wchar_t* verb = converting ? L"Converting" : L"Copying";
        wchar_t line[400];
        std::wstring result;
        { std::lock_guard<std::mutex> lock(copyLock); result = prepareResult; }
        if (state == 2 && preparing) swprintf(line, 400, L"Preparing %s\u2026  (making its game image)", title.c_str());
        else if (state == 1 || (state == 2 && !total)) swprintf(line, 400, L"%s %s\u2026  Getting ready", verb, title.c_str());
        else if (state == 2) {
            swprintf(line, 400, L"%s %s   %d%%   %.1f of %.1f GB   %s%s", verb, title.c_str(), (int)(fraction * 100),
                     done / 1073741824.0, total / 1073741824.0,
                     copySpeed > 0 ? (std::to_wstring((int)(copySpeed / 1048576)) + L" MB/s   ").c_str() : L"",
                     copySpeed > 0 ? FormatLeft((total - done) / copySpeed).c_str() : L"");
        } else if (state == 3 && converting) swprintf(line, 400, L"%s %s. %s", total ? L"Converted" : L"Prepared", title.c_str(), result.c_str());
        else if (state == 3) swprintf(line, 400, L"Copied %s. %s", title.c_str(), copyIsGame ? L"It's ready to play." : L"It's in the Import list.");
        else swprintf(line, 400, L"%s of %s stopped: %s", converting ? L"Converting" : L"Copy", title.c_str(), error.c_str());
        usbCopyLine.Text(line);
        if (state == 2 && now - progressLoggedAt >= std::chrono::seconds(5)) { progressLoggedAt = now; UsbLog(line); } // the line on screen, for the log
        // The Add Game page's progress row.
        wifiRowShown = false;
        progressRow.Visibility(Visibility::Visible);
        progressRow.Background(state == 4 ? Solid(70, 170, 44, 44) : Solid(64, 40, 130, 230));
        if (copyArt) {
            ImageBrush brush;
            brush.ImageSource(copyArt);
            brush.Stretch(Stretch::UniformToFill);
            progressArt.Background(brush);
        } else progressArt.Background(White(30));
        std::wstring head, side;
        if (state == 2 && preparing) head = L"Preparing " + title + L"\u2026";
        else if (state == 1 || state == 2) head = std::wstring(verb) + L" " + title;
        else if (state == 3 && converting) head = std::wstring(total ? L"Converted " : L"Prepared ") + title + L". " + result;
        else if (state == 3) head = L"Copied " + title + (copyIsGame ? L". It's ready to play." : L".");
        else head = std::wstring(converting ? L"Couldn't convert " : L"Couldn't copy ") + title + L". " + error;
        if (state == 2 && !preparing && total) {
            wchar_t text[160];
            swprintf(text, 160, L"%.1f of %.1f GB", done / 1073741824.0, total / 1073741824.0);
            side = text;
            if (copySpeed > 0) side += L"\n" + std::to_wstring((int)(copySpeed / 1048576)) + L" MB/s \u00B7 " + FormatLeft((total - done) / copySpeed);
        }
        progressTitle.Text(head);
        progressRight.Text(side);
        usbBarTrack.Visibility((state == 1 || state == 2) && !preparing ? Visibility::Visible : Visibility::Collapsed);
        SetBar(usbBar, fraction);
        for (size_t i = 0; i < games.size() && i < tileBars.size(); ++i) if (games[i].id == id && tileBars[i]) SetBar(tileBars[i], fraction);
        if (tab == 0 && selected < games.size() && games[selected].id == id && (state == 1 || state == 2)) {
            copyRow.Visibility(Visibility::Visible);
            SetBar(copyBar, fraction);
            copyLine.Text(line);
        }
    }

    void FinishCopy(int state) {
        // A finished game copy: the Xbox copy is used from now on, so a mount of the same game goes.
        std::wstring id, title;
        { std::lock_guard<std::mutex> lock(copyLock); id = copyId; title = copyTitle; }
        if (state == 3 && copyIsGame && !id.empty()) {
            auto mounts = ReadMounts();
            const size_t before = mounts.size();
            mounts.erase(std::remove_if(mounts.begin(), mounts.end(), [&](const Mount& m) { return m.id == id; }), mounts.end());
            if (mounts.size() != before) WriteMounts(mounts);
        }
        Rescan();
        Refresh(false);
        RefreshImport();
        ShowCopy();
        if (converting) {
            std::wstring error;
            { std::lock_guard<std::mutex> lock(copyLock); error = copyError; }
            if (state == 4 && shrinkOffer.exchange(false) && tab == 1 && !confirmOpen) {
                notice.Text(L"There isn't room on the drive for both the PKG and " + title + L".");
                confirmKind = 3;
                confirmOpen = true;
                Refresh(false);
                return;
            }
            notice.Text(state == 3 ? title + (convertUsedUp ? L": done. The PKG was used up." : L": done.") : L"Couldn't convert " + title + L". " + error);
            ScanUsb(); // the new game folder shows up in the list
            if (convertUsedUp) convertedPkgs.clear();
            if (state == 3 && convertPrepared && !convertedPkgs.empty() && tab == 1 && !confirmOpen) { // ask; never delete unasked
                confirmKind = 2;
                confirmOpen = true;
                Refresh(false);
            }
            return;
        }
        notice.Text(state == 3 ? L"Copied " + title + (copyIsGame ? L". It's ready to play." : L". It's in the Import list.")
                               : L"The copy of " + title + L" stopped. Copy it again to pick up where it stopped.");
    }

    void RefreshImport() {
        importList.Children().Clear();
        const auto files = ScanImport(dataRoot + L"\\Import");
        if (files.empty()) {
            importList.Children().Append(Text(L"None.", 13, White(170)));
            return;
        }
        for (const auto& p : files) {
            importList.Children().Append(Text(p.name + L"   " + FormatBytes(p.bytes) + (p.titleId.empty() ? L"   (not a readable PKG)" : L"   " + p.titleId), 13, White(210)));
        }
    }

    void ShowToast(const std::wstring& heading, const std::wstring& body, int seconds) {
        toastTitle.Text(heading);
        toastBody.Text(body);
        toast.Visibility(Visibility::Visible);
        toastShown = true;
        toastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        Anim::Storyboard board;
        Anim::DoubleAnimation fade;
        fade.From(0.0);
        fade.To(1.0);
        fade.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(280)));
        Anim::Storyboard::SetTarget(fade, toast);
        Anim::Storyboard::SetTargetProperty(fade, L"Opacity");
        Anim::DoubleAnimation slide;
        slide.From(-24.0);
        slide.To(0.0);
        slide.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(280)));
        Anim::Storyboard::SetTarget(slide, toastMove);
        Anim::Storyboard::SetTargetProperty(slide, L"Y");
        board.Children().Append(fade);
        board.Children().Append(slide);
        board.Begin();
    }

    void HideToast() {
        if (!toastShown) return;
        toastShown = false;
        Anim::Storyboard board;
        Anim::DoubleAnimation fade;
        fade.From(1.0);
        fade.To(0.0);
        fade.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(220)));
        Anim::Storyboard::SetTarget(fade, toast);
        Anim::Storyboard::SetTargetProperty(fade, L"Opacity");
        board.Children().Append(fade);
        board.Completed([this](auto const&, auto const&) { if (!toastShown) toast.Visibility(Visibility::Collapsed); });
        board.Begin();
    }

    // XStation (2026-11-13): says when the last game closed because it ran out of memory. The game writes
    // last-game.txt when it starts, and its memory guard writes Titles\<id>\last-exit-reason.txt just before it
    // closes the game. With no reason file, a last memory sample within 100 MiB of the limit means it most likely ran
    // out before the guard could act. Shown once: both files are removed here.
    void ShowLastExit() {
        std::wstring id;
        { std::wifstream last(local + L"\\last-game.txt"); std::getline(last, id); }
        DeleteFileW((local + L"\\last-game.txt").c_str());
        while (!id.empty() && (id.back() == L'\r' || id.back() == L' ')) id.pop_back();
        if (id.empty()) return;
        for (const wchar_t c : id) if (!iswalnum(c)) return;
        const std::wstring folder = local + L"\\Titles\\" + id;
        std::wstring name = id;
        for (const auto& g : games) if (g.id == id) name = g.title;
        unsigned long long used = 0, limit = 0;
        std::string kind;
        bool sure = false;
        {
            std::ifstream why(folder + L"\\last-exit-reason.txt");
            sure = bool(why >> kind >> used >> limit) && kind == "memory_guard" && limit;
        }
        DeleteFileW((folder + L"\\last-exit-reason.txt").c_str());
        if (!sure) {
            used = limit = 0;
            std::ifstream csv(folder + L"\\app-memory.csv", std::ios::binary);
            if (!csv) return;
            csv.seekg(0, std::ios::end);
            const long long size = static_cast<long long>(csv.tellg());
            csv.seekg(size > 8192 ? size - 8192 : 0);
            std::vector<std::string> rows;
            for (std::string row; std::getline(csv, row);) rows.push_back(row);
            // the last whole row (the file can end part way through one)
            for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                unsigned long long tick = 0, u = 0, l = 0;
                if (std::count(it->begin(), it->end(), ',') >= 13 && sscanf_s(it->c_str(), "%llu,%llu,%llu,", &tick, &u, &l) == 3 && l) {
                    used = u >> 20; limit = l >> 20;
                    break;
                }
            }
            if (!limit || used + 100 < limit) return;
        }
        UsbLog(L"last exit " + id + (sure ? L" memory guard " : L" near limit ") + std::to_wstring(used) + L" / " + std::to_wstring(limit));
        ShowToast(sure ? name + L" closed: out of memory" : name + L" most likely closed: out of memory",
                  L"Last time it was using " + std::to_wstring(used) + L" of the Xbox's " + std::to_wstring(limit) +
                      L" MiB game memory limit" + (sure ? L", so XStation closed it before the console would." : L"."),
                  15);
    }

    void Move(int direction) {
        if (tab != 0 || detailsOpen || confirmOpen || deleting || games.empty()) return;
        const size_t next = direction < 0 ? (selected ? selected - 1 : selected) : (selected + 1 < games.size() ? selected + 1 : selected);
        if (next == selected) return;
        selected = next;
        notice.Text(L"");
        Refresh();
    }

    void Play() {
        if (launching || deleting || confirmOpen || tab != 0 || detailsOpen || games.empty()) {
            UsbLog(L"play ignored: launching " + std::to_wstring(launching) + L" deleting " + std::to_wstring((bool)deleting) + L" confirm " +
                   std::to_wstring(confirmOpen) + L" tab " + std::to_wstring(tab) + L" details " + std::to_wstring(detailsOpen) + L" games " + std::to_wstring(games.size()));
            return;
        }
        const Game& g = games[selected];
        UsbLog(L"play " + g.id + L" folder " + g.folder + L" mounted " + std::to_wstring(g.mounted) + L" usbMissing " + std::to_wstring(g.usbMissing) +
               L" copying " + std::to_wstring(g.copying) + L" boot " + std::to_wstring(g.hasBoot) + L" engine " + std::to_wstring(g.hasEngine) +
               L" copyState " + std::to_wstring(copyState.load()));
        if (CopyRunning()) { notice.Text(L"A game is copying from USB. Starting a game now would stop the copy, so wait for it to finish."); return; }
        if (RunsAsApp()) {
            notice.Text(kSetAsGame);
            UsbLog(L"play refused: memory limit " + std::to_wstring(Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20) + L" MiB (set as App)");
            return;
        }
        if (g.usbMissing || (g.mounted && !Exists(g.folder))) {
            notice.Text(L"Plug in the USB drive that has " + g.title + L".");
            WakeUsb();
            return;
        }
        if (g.copying) { notice.Text(L"This game's copy didn't finish. Copy it again from the USB drive on the Add Game page."); return; }
        if (!g.hasBoot || !g.hasEngine) {
            notice.Text(g.hasEngine ? L"This game isn't prepared yet. Delete it, then add its PKG again on the Add Game page (USB or Wi-Fi): XStation converts and prepares it." : L"XStation has no engine for this game yet.");
            return;
        }
        // 2026-11-14: The Last of Us needs its reduced home textures and shader packs (made at prepare time); a game
        // prepared before that, or whose files were replaced, gets them here once, then starts.
        // Uncharted likewise: its 25 texture dictionaries trimmed (several minutes, once).
        const bool tlouAssets = GameEngineId(g.id) == L"CUSA00552" && TlouAssetsNeeded(g.folder);
        const bool ucAssets = GameEngineId(g.id) == L"CUSA02343" && UnchartedAssetsNeeded(g.folder);
        if (tlouAssets || ucAssets) {
            launching = true;
            notice.Text(tlouAssets ? L"Getting The Last of Us ready: smaller home textures and shader packs from your game files. This takes a few minutes, once."
                                   : L"Getting Uncharted ready: smaller texture files from your game files. This takes several minutes, once.");
            const auto dispatcher = Window::Current().Dispatcher();
            std::thread([this, keep = get_strong(), dispatcher, tlouAssets, id = g.id, title = g.title, folder = g.folder] {
                std::string detail;
                const auto began = std::chrono::steady_clock::now();
                if (tlouAssets) TlouPrepareAssets(folder, detail);
                else UnchartedTrimTextures(folder, detail);
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                UsbLog(L"game assets " + folder + L" in " + std::to_wstring((int)s) + L" s\n" + FromUtf8(detail));
                dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [this, keep, id, title] {
                    launching = false;
                    StartGame(id, title);
                });
            }).detach();
            return;
        }
        StartGame(g.id, g.title);
    }

    void StartGame(const std::wstring& id, const std::wstring& title) {
        {
            std::wofstream request(local + L"\\play.txt", std::ios::trunc);
            request << id;
        }
        launching = true;
        notice.Text(L"Starting " + title + L"…");
        auto operation = Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(L"");
        operation.Completed([this](auto const& op, auto) {
            const auto result = op.GetResults();
            UsbLog(L"restart request result " + std::to_wstring((int)result));
            if (result != Windows::ApplicationModel::Core::AppRestartFailureReason::RestartPending) {
                // play.txt stays: the next launch of XStation starts the game.
                Window::Current().Dispatcher().RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [] { Application::Current().Exit(); });
            }
        });
    }

    // The host reads the file when the game starts. A folder for a game not played yet is made here.
    void ToggleFps() {
        if (tab != 0 || detailsOpen || confirmOpen || deleting || launching || games.empty()) return;
        Game& g = games[selected];
        if (!HasFpsChoice(g.id)) return;
        if (HasFsrChoice(g.id)) {
            if (!g.fsr) { confirmKind = 1; confirmOpen = true; Refresh(false); return; } // the warning first
            SetFsr(false);
            return;
        }
        const int wanted = g.fps == 60 ? 30 : 60;
        const std::wstring folder = local + L"\\Titles\\" + g.id;
        CreateDirectoryW((local + L"\\Titles").c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        // No file means 60 for every engine; Bloodborne's reads the file itself and knows only 30.
        if (wanted == 30) {
            std::ofstream limit(folder + L"\\frame-limit.txt", std::ios::trunc);
            limit << 30;
            if (!limit.flush()) { notice.Text(L"Couldn't save the FPS choice."); return; }
        } else if (!DeleteFileW((folder + L"\\frame-limit.txt").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            notice.Text(L"Couldn't save the FPS choice.");
            return;
        }
        g.fps = wanted;
        fpsLabel.Text(std::to_wstring(g.fps) + L" FPS");
        notice.Text(g.title + L" will run at up to " + std::to_wstring(g.fps) + L" FPS.");
    }

    // FSR games: 30 = frame-limit 30 and no frame-gen.txt; 60 (FSR) = frame-limit 30 (the game itself) and frame-gen.txt 1.
    void SetFsr(bool on) {
        Game& g = games[selected];
        const std::wstring folder = local + L"\\Titles\\" + g.id;
        CreateDirectoryW((local + L"\\Titles").c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        {
            std::ofstream limit(folder + L"\\frame-limit.txt", std::ios::trunc);
            limit << 30;
            if (!limit.flush()) { notice.Text(L"Couldn't save the FPS choice."); return; }
        }
        if (on) {
            std::ofstream frameGen(folder + L"\\frame-gen.txt", std::ios::trunc);
            frameGen << 1;
            if (!frameGen.flush()) { notice.Text(L"Couldn't save the FPS choice."); return; }
        } else if (!DeleteFileW((folder + L"\\frame-gen.txt").c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            notice.Text(L"Couldn't save the FPS choice.");
            return;
        }
        g.fsr = on;
        g.fps = on ? 60 : 30;
        fpsLabel.Text(std::to_wstring(g.fps) + L" FPS" + (g.fsr ? L" (FSR)" : L""));
        fpsButton.Width(g.fsr ? 150 : 96);
        notice.Text(on ? g.title + L" will use FSR: about 60 FPS on screen, the game itself at about 30."
                       : g.title + L" will run at 30 FPS.");
    }

    // FSR upscaling (HasFsrUpscale games): fsr-upscale.txt = 1 or 0; the host reads it when the game starts.
    void ToggleUpscale() {
        if (tab != 0 || detailsOpen || confirmOpen || deleting || launching || games.empty()) return;
        Game& g = games[selected];
        if (!HasFsrUpscale(g.id)) return;
        const bool on = !g.upscale;
        const std::wstring folder = local + L"\\Titles\\" + g.id;
        CreateDirectoryW((local + L"\\Titles").c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        // 2026-11-10 (user): 540p is the default, FSR an option. The game patch draws 960x540 whatever the scene setting ("720p" only added a white streak), so scene stays 540p.
        std::ofstream file(folder + L"\\fsr-upscale.txt", std::ios::trunc);
        file << (on ? 1 : 0);
        std::ofstream scene(folder + L"\\scene-resolution.txt", std::ios::trunc);
        scene << "540p";
        if (!file.flush() || !scene.flush()) { notice.Text(L"Couldn't save the FSR choice."); return; }
        g.upscale = on;
        upscaleLabel.Text(on ? L"FSR On" : L"FSR Off");
        notice.Text(on ? L"FSR on: the game's 960x540 picture is upscaled by FSR to 1920x1080. Sharper, about 300-600 MB more memory."
                       : L"FSR off: the game's own 960x540 picture. Softer, and about 300-600 MB less memory.");
    }

    // XStation (2026-11-13): the engine's FPS and CPU box on or off, every game (in Details, Y, with Menu): cpu-counter.txt = 1 or 0; the host
    // writes it into the title's bbport.ini (show_fps) when the game starts.
    void ToggleCpu() {
        if (tab != 0 || !detailsOpen || confirmOpen || deleting || launching || games.empty()) return;
        Game& g = games[selected];
        const bool on = !g.cpu;
        const std::wstring folder = local + L"\\Titles\\" + g.id;
        CreateDirectoryW((local + L"\\Titles").c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        std::ofstream file(folder + L"\\cpu-counter.txt", std::ios::trunc);
        file << (on ? 1 : 0);
        if (!file.flush()) { notice.Text(L"Couldn't save the CPU counter choice."); return; }
        g.cpu = on;
        Refresh(false);
        notice.Text(on ? g.title + L" will show the CPU counter (FPS and CPU use per core) in the top right corner."
                       : g.title + L" won't show the CPU counter. XStation's memory and FPS line stays.");
    }

    // XStation (2026-11-15): View in Details copies the game's saves to <USB>\XStation\Saves\<id>\<date time>.
    fire_and_forget ExportSaves() {
        if (tab != 0 || !detailsOpen || confirmOpen || deleting || launching || savesBusy || games.empty()) co_return;
        auto keep = get_strong();
        const Game g = games[selected];
        const std::wstring from = SaveFolder(local, g.id);
        if (!IsFolderFromApp(from)) { notice.Text(g.title + L" has no saves yet."); co_return; }
        savesBusy = true;
        notice.Text(L"Exporting " + g.title + L" saves to the USB drive…");
        std::wstring root;
        try {
            auto drives = co_await KnownFolders::RemovableDevices().GetFoldersAsync();
            for (auto const& drive : drives) { root = drive.Path().c_str(); break; }
        } catch (...) {}
        while (!root.empty() && root.back() == L'\\') root.pop_back();
        if (root.empty()) { savesBusy = false; notice.Text(L"No USB drive found. Plug one in and try again."); co_return; }
        const auto dispatcher = Window::Current().Dispatcher();
        std::thread([this, keep, dispatcher, g, from, root] {
            const std::wstring game = root + L"\\XStation\\Saves\\" + g.id, stamp = SaveStamp(), to = game + L"\\" + stamp;
            CreateDirectoryFromAppW((root + L"\\XStation").c_str(), nullptr);
            CreateDirectoryFromAppW((root + L"\\XStation\\Saves").c_str(), nullptr);
            CreateDirectoryFromAppW(game.c_str(), nullptr);
            uint64_t files = 0, bytes = 0;
            std::wstring error;
            bool ok = CreateDirectoryFromAppW(to.c_str(), nullptr) != 0;
            if (!ok) error = L"make folder " + to + L" (error " + std::to_wstring(GetLastError()) + L")";
            if (ok) ok = CopyTreeFromApp(from, to + L"\\savedata", files, bytes, error);
            if (ok) {
                std::ofstream info(to + L"\\info.txt", std::ios::trunc | std::ios::binary);
                info << ToUtf8(g.title) << "\r\n" << ToUtf8(g.id) << "\r\n" << files << " files, " << bytes << " bytes\r\n"
                     << "from " << ToUtf8(from.substr(local.size())) << "\r\n";
                ok = bool(info.flush());
                if (!ok) error = L"write info.txt";
            }
            // 2026-11-15: a failed export leaves nothing behind (0.1.2.2 left an empty date folder per try).
            if (!ok) { DeleteTreeFromApp(to); RemoveDirectoryFromAppW(game.c_str()); }
            UsbLog(L"save export " + g.id + L" -> " + to + (ok ? L" ok " : L" FAILED ") + std::to_wstring(files) + L" files" +
                   (error.empty() ? L"" : L": " + error));
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [this, keep, g, stamp, files, bytes, ok, error] {
                savesBusy = false;
                SaveStatus(ok ? L"Exported " + std::to_wstring(files) + L" save files (" + FormatBytes(bytes) + L") to the USB drive: XStation\\Saves\\" +
                                    g.id + L"\\" + stamp
                              : L"Couldn't export the saves to the USB drive: " + error + L". Your saves on the Xbox weren't changed.", ok);
            });
        }).detach();
    }

    fire_and_forget ExportLogs() {
        if (tab != 0 || !detailsOpen || confirmOpen || deleting || launching || savesBusy || games.empty()) co_return;
        auto keep = get_strong();
        const Game g = games[selected];
        savesBusy = true;
        notice.Text(L"Copying " + g.title + L" logs to the USB drive…");
        std::wstring root;
        try {
            auto drives = co_await KnownFolders::RemovableDevices().GetFoldersAsync();
            for (auto const& drive : drives) { root = drive.Path().c_str(); break; }
        } catch (...) {}
        while (!root.empty() && root.back() == L'\\') root.pop_back();
        if (root.empty()) { savesBusy = false; UsbLog(L"log export " + g.id + L": no USB drive"); notice.Text(L"No USB drive found. Plug one in and try again."); co_return; }
        std::string system;
        {
            const auto v = Windows::ApplicationModel::Package::Current().Id().Version();
            const auto info = Windows::System::Profile::AnalyticsInfo::VersionInfo();
            system += "XStation " + std::to_string(v.Major) + "." + std::to_string(v.Minor) + "." + std::to_string(v.Build) + "." +
                      std::to_string(v.Revision) + "\r\n";
            system += "device family " + ToUtf8(info.DeviceFamily().c_str()) + " " + ToUtf8(info.DeviceFamilyVersion().c_str()) + "\r\n";
            try {
                Windows::Security::ExchangeActiveSyncProvisioning::EasClientDeviceInformation device;
                system += "console " + ToUtf8(device.SystemManufacturer().c_str()) + " " + ToUtf8(device.SystemProductName().c_str()) + " " +
                          ToUtf8(device.SystemSku().c_str()) + "\r\n";
            } catch (...) { system += "console unknown\r\n"; }
            system += "dashboard memory limit MiB " + std::to_string(Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20) + "\r\n";
            ULARGE_INTEGER freeBytes{};
            if (GetDiskFreeSpaceExW((dataRoot + L"\\").c_str(), &freeBytes, nullptr, nullptr))
                system += "free on " + ToUtf8(dataRoot) + ": " + ToUtf8(FormatBytes(freeBytes.QuadPart)) + "\r\n";
            system += "game " + ToUtf8(g.title) + " " + ToUtf8(g.id) + " engine " + ToUtf8(GameEngineId(g.id)) + "\r\n";
        }
        const auto dispatcher = Window::Current().Dispatcher();
        std::thread([this, keep, dispatcher, g, root, system] {
            const std::wstring logs = root + L"\\XStation\\Logs", game = logs + L"\\" + g.id, stamp = SaveStamp(), to = game + L"\\" + stamp;
            CreateDirectoryFromAppW((root + L"\\XStation").c_str(), nullptr);
            CreateDirectoryFromAppW(logs.c_str(), nullptr);
            CreateDirectoryFromAppW(game.c_str(), nullptr);
            uint64_t files = 0, bytes = 0;
            std::string skipped;
            std::wstring error;
            bool ok = CreateDirectoryFromAppW(to.c_str(), nullptr) != 0;
            if (!ok) error = L"make folder " + to + L" (error " + std::to_wstring(GetLastError()) + L")";
            // Copies the log-type files directly in `from` (no subfolders) into to\<sub>.
            auto copyLogs = [&](const std::wstring& from, const std::wstring& sub) {
                WIN32_FIND_DATAW f;
                HANDLE h = FindFirstFileExFromAppW((from + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
                if (h == INVALID_HANDLE_VALUE) return;
                bool made = false;
                do {
                    const std::wstring name = f.cFileName;
                    if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    const auto dot = name.rfind(L'.');
                    std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
                    for (auto& c : ext) c = towlower(c);
                    if (ext != L".log" && ext != L".txt" && ext != L".csv" && ext != L".ini") continue;
                    const uint64_t size = (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
                    if (size > (64ull << 20)) { skipped += ToUtf8(sub + L"\\" + name) + " (too big, " + std::to_string(size >> 20) + " MiB)\r\n"; continue; }
                    if (!made) { CreateDirectoryFromAppW((to + L"\\" + sub).c_str(), nullptr); made = true; }
                    std::wstring step;
                    if (CopyFileThroughApp(from + L"\\" + name, to + L"\\" + sub + L"\\" + name, bytes, step)) ++files;
                    else skipped += ToUtf8(sub + L"\\" + name) + " (" + ToUtf8(step) + ")\r\n";
                } while (FindNextFileW(h, &f));
                FindClose(h);
            };
            if (ok) {
                const std::wstring title = local + L"\\Titles\\" + g.id;
                copyLogs(title, L"game");
                for (const wchar_t* user : {L"game-user", L"tlou-bb194-user"}) copyLogs(title + L"\\" + user, std::wstring(L"game\\") + user);
                copyLogs(dataRoot, L"xstation");
                if (dataRoot != local) copyLogs(local, L"xstation-app");
                std::ofstream info(to + L"\\system.txt", std::ios::trunc | std::ios::binary);
                info << system << files << " log files, " << bytes << " bytes\r\n";
                if (!skipped.empty()) info << "skipped:\r\n" << skipped;
                ok = bool(info.flush());
                if (!ok) error = L"write system.txt";
            }
            if (!ok) { DeleteTreeFromApp(to); RemoveDirectoryFromAppW(game.c_str()); }
            UsbLog(L"log export " + g.id + L" -> " + to + (ok ? L" ok " : L" FAILED ") + std::to_wstring(files) + L" files" +
                   (error.empty() ? L"" : L": " + error));
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [this, keep, g, stamp, files, bytes, ok, error] {
                savesBusy = false;
                SaveStatus(ok ? L"Copied " + std::to_wstring(files) + L" log files (" + FormatBytes(bytes) + L") to the USB drive: XStation\\Logs\\" +
                                    g.id + L"\\" + stamp + L". Send that folder with your bug report."
                              : L"Couldn't copy the logs to the USB drive: " + error + L".", ok);
            });
        }).detach();
    }

    // X in Details: find this game's exports on the USB drive and ask which one to import (X again: the next older).
    fire_and_forget AskImport() {
        if (tab != 0 || !detailsOpen || confirmOpen || deleting || launching || savesBusy || games.empty()) co_return;
        auto keep = get_strong();
        const Game g = games[selected];
        savesBusy = true;
        notice.Text(L"Looking for saved exports on the USB drive…");
        std::wstring root;
        try {
            auto drives = co_await KnownFolders::RemovableDevices().GetFoldersAsync();
            for (auto const& drive : drives) { root = drive.Path().c_str(); break; }
        } catch (...) {}
        while (!root.empty() && root.back() == L'\\') root.pop_back();
        savesBusy = false;
        if (root.empty()) { notice.Text(L"No USB drive found. Plug in the drive with the exported saves and try again."); co_return; }
        std::vector<std::wstring> found;
        const std::wstring game = root + L"\\XStation\\Saves\\" + g.id;
        WIN32_FIND_DATAW f;
        HANDLE h = FindFirstFileExFromAppW((game + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = f.cFileName;
                if (name != L"." && name != L".." && (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsFolderFromApp(game + L"\\" + name + L"\\savedata"))
                    found.push_back(name);
            } while (FindNextFileW(h, &f));
            FindClose(h);
        }
        if (found.empty()) { notice.Text(L"No exported saves for " + g.title + L" on the USB drive (looked in XStation\\Saves\\" + g.id + L")."); co_return; }
        std::sort(found.begin(), found.end(), std::greater<>()); // the names are dates: newest first
        if (!detailsOpen || confirmOpen || selected >= games.size() || games[selected].id != g.id) co_return;
        importChoices = found;
        importPick = 0;
        importRoot = root;
        confirmKind = 4;
        confirmOpen = true;
        notice.Text(L"");
        Refresh(false);
    }

    void NextImport() {
        if (!confirmOpen || confirmKind != 4 || importChoices.size() < 2) return;
        importPick = (importPick + 1) % importChoices.size();
        Refresh(false);
    }

    void ConfirmImport() {
        if (!confirmOpen || confirmKind != 4 || selected >= games.size() || importPick >= importChoices.size()) return;
        confirmOpen = false;
        confirmKind = 0;
        const Game g = games[selected];
        const std::wstring from = importRoot + L"\\XStation\\Saves\\" + g.id + L"\\" + importChoices[importPick] + L"\\savedata",
                           choice = importChoices[importPick];
        savesBusy = true;
        notice.Text(L"Importing " + g.title + L" saves from the USB drive…");
        Refresh(false);
        const auto dispatcher = Window::Current().Dispatcher();
        std::thread([this, keep = get_strong(), dispatcher, g, from, choice] {
            const std::wstring to = SaveFolder(local, g.id), title = local + L"\\Titles\\" + g.id;
            const std::wstring backups = title + L"\\save-backups", backup = backups + L"\\" + SaveStamp();
            std::wstring error;
            bool moved = false;
            uint64_t files = 0, bytes = 0;
            CreateDirectoryFromAppW((local + L"\\Titles").c_str(), nullptr);
            CreateDirectoryFromAppW(title.c_str(), nullptr);
            CreateDirectoryFromAppW(to.substr(0, to.find_last_of(L'\\')).c_str(), nullptr); // the engine's user folder
            if (!IsFolderFromApp(from)) error = L"The export isn't on the USB drive any more.";
            if (error.empty() && IsFolderFromApp(to)) {
                CreateDirectoryFromAppW(backups.c_str(), nullptr);
                CreateDirectoryFromAppW(backup.c_str(), nullptr);
                if (MoveFileFromAppW(to.c_str(), (backup + L"\\savedata").c_str())) moved = true;
                else error = L"Couldn't set the current saves aside, so nothing was changed.";
            }
            std::wstring step;
            if (error.empty() && !CopyTreeFromApp(from, to, files, bytes, step)) {
                DeleteTreeFromApp(to); // only the partial copy just made
                if (moved && MoveFileFromAppW((backup + L"\\savedata").c_str(), to.c_str())) moved = false;
                error = L"The copy failed (" + step + L"). " + (moved ? L"Your previous saves are in Titles\\" + g.id + L"\\save-backups."
                                                                      : L"Your previous saves were put back.");
            }
            UsbLog(L"save import " + g.id + L" <- " + from + (error.empty() ? L" ok " : L" FAILED ") + std::to_wstring(files) + L" files" +
                   (moved ? L", previous in " + backup : L""));
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [this, keep, g, choice, files, moved, error] {
                savesBusy = false;
                SaveStatus(!error.empty() ? error
                                          : L"Imported " + std::to_wstring(files) + L" save files for " + g.title + L" from " + choice + L"." +
                                                (moved ? L" The previous saves are kept in Titles\\" + g.id + L"\\save-backups." : L""),
                           error.empty());
                Refresh(false);
            });
        }).detach();
    }

    static bool ParseVersion(const std::wstring& text, uint64_t& out) {
        size_t at = 0;
        while (at < text.size() && !iswdigit(text[at])) ++at;
        uint64_t parts[4] = {};
        int n = 0;
        for (; n < 4 && at < text.size() && iswdigit(text[at]); ++n) {
            uint64_t v = 0;
            while (at < text.size() && iswdigit(text[at])) v = v * 10 + (text[at++] - L'0');
            parts[n] = v > 0xffff ? 0xffff : v;
            if (at < text.size() && text[at] == L'.') ++at; else { ++n; break; }
        }
        if (n < 2) return false;
        out = (parts[0] << 48) | (parts[1] << 32) | (parts[2] << 16) | parts[3];
        return true;
    }

    static std::wstring NotesText(std::wstring s) {
        std::wstring out;
        size_t lines = 0;
        size_t at = 0;
        while (at <= s.size() && lines < 18) {
            size_t end = s.find(L'\n', at);
            if (end == std::wstring::npos) end = s.size();
            std::wstring line = s.substr(at, end - at);
            at = end + 1;
            while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
            size_t lead = 0;
            while (lead < line.size() && (line[lead] == L'#' || line[lead] == L' ')) ++lead;
            line = line.substr(lead);
            for (size_t k; (k = line.find(L"**")) != std::wstring::npos;) line.erase(k, 2);
            for (size_t k; (k = line.find(L"](")) != std::wstring::npos;) { // [text](link) -> text
                const size_t open = line.rfind(L'[', k), close = line.find(L')', k);
                if (open == std::wstring::npos || close == std::wstring::npos) break;
                line = line.substr(0, open) + line.substr(open + 1, k - open - 1) + line.substr(close + 1);
            }
            if (line.size() > 1 && (line[0] == L'-' || line[0] == L'*') && line[1] == L' ') line = L"\u2022 " + line.substr(2);
            if (line.empty() && (out.empty() || out.back() == L'\n')) continue;
            out += line + L"\n";
            ++lines;
        }
        while (!out.empty() && out.back() == L'\n') out.pop_back();
        if (at < s.size()) out += L"\n\u2026 (more on the release page)";
        if (out.size() > 1400) out = out.substr(0, 1400) + L"\u2026";
        return out.empty() ? L"(no notes)" : out;
    }

    static std::wstring Today() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t text[16];
        swprintf(text, 16, L"%04d%02d%02d", t.wYear, t.wMonth, t.wDay);
        return text;
    }

    void UpdateSeen() { // asked today for this release: not again until tomorrow
        std::wofstream seen(local + L"\\update-seen.txt", std::ios::trunc);
        seen << updateTag << L"\n" << Today() << L"\n";
    }

    void OpenUpdate() {
        if (!confirmOpen || confirmKind != 5) return;
        UpdateSeen();
        confirmOpen = false;
        confirmKind = 0;
        Refresh(false);
        if (!updateUrl.empty()) Windows::System::Launcher::LaunchUriAsync(Uri(updateUrl));
    }

    fire_and_forget CheckForUpdate() {
        auto keep = get_strong();
        const auto v = Windows::ApplicationModel::Package::Current().Id().Version();
        const uint64_t mine = updateTestDev ? 0 : (uint64_t(v.Major) << 48) | (uint64_t(v.Minor) << 32) | (uint64_t(v.Build) << 16) | v.Revision;
        const std::wstring mineText = std::to_wstring(v.Major) + L"." + std::to_wstring(v.Minor) + L"." + std::to_wstring(v.Build) + L"." +
                                      std::to_wstring(v.Revision);
        std::wstring seenTag, seenDay;
        { std::wifstream seen(local + L"\\update-seen.txt"); std::getline(seen, seenTag); std::getline(seen, seenDay); }
        co_await resume_background();
        try {
            Windows::Web::Http::HttpClient http;
            http.DefaultRequestHeaders().UserAgent().TryParseAdd(L"XStation-UWP");
            http.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");
            // The release list, not releases/latest: that one skips pre-releases, and the repo's releases are "Preview"
            // pre-releases. The highest tag wins; drafts are not listed to others anyway.
            const hstring body = co_await http.GetStringAsync(Uri(L"https://api.github.com/repos/BBCPrince/XStation-UWP/releases?per_page=20"));
            const auto list = Windows::Data::Json::JsonArray::Parse(body);
            Windows::Data::Json::JsonObject j{nullptr};
            uint64_t theirs = 0;
            auto field = [](Windows::Data::Json::JsonObject const& o, const wchar_t* key) -> std::wstring {
                if (!o.HasKey(key)) return L"";
                const auto value = o.GetNamedValue(key);
                return value.ValueType() == Windows::Data::Json::JsonValueType::String ? std::wstring(value.GetString()) : L"";
            };
            for (const auto& item : list) {
                if (item.ValueType() != Windows::Data::Json::JsonValueType::Object) continue;
                const auto o = item.GetObject();
                if (o.HasKey(L"draft") && o.GetNamedValue(L"draft").ValueType() == Windows::Data::Json::JsonValueType::Boolean && o.GetNamedBoolean(L"draft")) continue;
                uint64_t version = 0;
                if (ParseVersion(field(o, L"tag_name"), version) && version > theirs) { theirs = version; j = o; }
            }
            if (!j) co_return;
            auto text = [&](const wchar_t* key) { return field(j, key); };
            const std::wstring tag = text(L"tag_name"), url = text(L"html_url");
            if (tag.empty() || url.rfind(L"https://github.com/", 0) != 0 || theirs <= mine) co_return;
            std::wstring shown = tag;
            if (!shown.empty() && (shown[0] == L'v' || shown[0] == L'V')) shown.erase(0, 1);
            if (!updateTestDev && seenTag == shown && seenDay == Today()) co_return; // "later" today
            const std::wstring name = text(L"name"), notes = NotesText(text(L"body"));
            {
                std::lock_guard<std::mutex> lock(copyLock);
                updateTag = shown;
                updateMine = mineText;
                updateNotes = (name.empty() || name == tag ? L"" : name + L"\n") + notes;
                updateUrl = url;
            }
            updateReady = true;
        } catch (...) {
        }
    }

    void ConfirmFsr() {
        if (!confirmOpen || confirmKind != 1 || selected >= games.size()) return;
        confirmOpen = false;
        confirmKind = 0;
        Refresh(false);
        SetFsr(true);
    }

    void Back() {
        if (toastShown) { HideToast(); return; }
        if (confirmOpen) {
            if (confirmKind == 5) UpdateSeen();
            confirmOpen = false;
            if (confirmKind == 4 || confirmKind == 5) confirmKind = 0;
            Refresh(false);
            return;
        }
        if (detailsOpen) { detailsOpen = false; Refresh(false); return; }
        if (tab != 0) { tab = 0; Refresh(); }
    }

    void SwitchTab(int to) {
        to = std::clamp(to, 0, 2); // Games, Add Game, Compatible (2026-11-18)
        if (detailsOpen || confirmOpen || tab == to) return;
        tab = to;
        Refresh();
    }

    void Details() {
        if (tab != 0 || games.empty()) return;
        detailsOpen = !detailsOpen;
        if (detailsOpen && !savesBusy) SaveStatus(L"", true);
        Refresh(false);
    }

    void Poll() {
        // Clock
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t text[32];
        const int hour = now.wHour % 12 ? now.wHour % 12 : 12;
        swprintf(text, 32, L"%d:%02d %s", hour, now.wMinute, now.wHour < 12 ? L"AM" : L"PM");
        if (clock.Text() != text) clock.Text(text);
        if (toastShown && std::chrono::steady_clock::now() >= toastUntil) HideToast();
        {
            const TransferState t = GetTransferState();
            if (tab == 1 && wifiStarted && wifiCodeText.Text() != L"\u2014") {
                wchar_t line[256] = L"Waiting for a PC";
                const bool converting = t.streaming && t.streamPhase == 1 && t.convertTotal;
                if (converting)
                    swprintf(line, 256, L"Converting while it arrives: %.1f of %.1f GB written", t.convertDone / 1073741824.0, t.convertTotal / 1073741824.0);
                else if (t.streaming) swprintf(line, 256, L"Receiving a PKG: %.2f GB%s", t.received / 1073741824.0, t.streamPhase == 2 ? L" (kept as a PKG)" : L"");
                else if (t.active) swprintf(line, 256, L"Receiving: %.2f GB, %d files done", t.received / 1073741824.0, t.finished);
                else if (!t.lastPkgMessage.empty()) swprintf(line, 256, L"%s", t.lastPkgMessage.c_str());
                else if (t.received) swprintf(line, 256, L"Done: %.2f GB received. It's in Games.", t.received / 1073741824.0);
                if (wifiStatusText.Text() != line) wifiStatusText.Text(line);
                if (copyState == 0 && t.active) {
                    wifiRowShown = true;
                    progressRow.Visibility(Visibility::Visible);
                    progressArt.Background(White(30));
                    if (converting) {
                        progressTitle.Text(L"Converting " + (t.streamTitle.empty() ? t.name : t.streamTitle) + L" from your PC");
                        wchar_t side[120];
                        swprintf(side, 120, L"%.1f of %.1f GB\nPKG on the drive: %.1f GB", t.convertDone / 1073741824.0, t.convertTotal / 1073741824.0,
                                 t.streamStored / 1073741824.0);
                        progressRight.Text(side);
                        usbBarTrack.Visibility(Visibility::Visible);
                        SetBar(usbBar, double(t.convertDone) / double(t.convertTotal));
                    } else {
                        progressTitle.Text(L"Receiving from your PC" + (t.name.empty() ? L"" : L": " + t.name));
                        wchar_t side[80];
                        swprintf(side, 80, L"%.2f GB \u00B7 %d files", t.received / 1073741824.0, t.finished);
                        progressRight.Text(side);
                        usbBarTrack.Visibility(Visibility::Collapsed);
                    }
                } else if (wifiRowShown && !t.active) {
                    wifiRowShown = false;
                    progressRow.Visibility(Visibility::Collapsed);
                }
            }
            // A PKG converted over Wi-Fi: prepare its image like a USB conversion, then show it in the library.
            if (t.conversions != wifiConversionsSeen) {
                wifiConversionsSeen = t.conversions;
                const std::wstring id = t.convertedId, target = t.convertedTarget;
                std::thread([this, keep = get_strong(), id, target] {
                    std::wstring message;
                    if (true) { // 2026-11-17: any game is checked against every engine (other regions)
                        std::string detail;
                        std::wstring perr, engine;
                        const bool prepared = GamePrepareAny(id, target, package + L"\\Engines", engine, perr, detail);
                        UsbLog(L"wifi prepare " + id + L": " + (prepared ? L"done" : perr) + L"\n" + FromUtf8(detail));
                        message = prepared ? id + L" is converted and ready to play." : id + L" is converted, but it couldn't be prepared: " + perr;
                    } else message = id + L" is converted. XStation can't prepare this game on the console yet.";
                    { std::lock_guard<std::mutex> lock(copyLock); wifiPrepareMessage = message; }
                    wifiPrepared = true;
                }).detach();
            }
            if (wifiPrepared.exchange(false)) {
                std::wstring message;
                { std::lock_guard<std::mutex> lock(copyLock); message = wifiPrepareMessage; }
                notice.Text(message);
                Rescan();
                Refresh(false);
            }
            if (wifiWasActive && !t.active && t.finished) { Rescan(); Refresh(false); } // a game sent from the PC
            wifiWasActive = t.active;
        }

        if (updateReady && tab == 0 && !confirmOpen && !detailsOpen && !launching && !deleting && !toastShown) {
            updateReady = false;
            confirmKind = 5;
            confirmOpen = true;
            Refresh(false);
        }
        if (--storageWait <= 0) { storageWait = 150; ShowStorage(); if (MountMissing()) WakeUsb(); } // every 5 s: transfers and deletes change it
        {
            const int state = copyState;
            if (state != shownCopyState && (state == 3 || state == 4)) { shownCopyState = state; FinishCopy(state); }
            else if (state != shownCopyState) { shownCopyState = state; ShowCopy(); }
            if (copyIsGame && copyArtReady.exchange(false) && CopyRunning()) { Rescan(); Refresh(false); } // the tile, with its art
            if (mountScanReady.exchange(false) && !launching && !deleting) { Rescan(); Refresh(false); } // mounted games read
            if ((state == 1 || state == 2) && --progressWait <= 0) { progressWait = 8; ShowCopy(); }
        }
        if (deleting) {
            if (deleteFinished) {
                deleting = false;
                const bool failed = deleteFailures.load() != 0;
                detailsOpen = false;
                Rescan();
                Refresh();
                notice.Text(failed ? L"Some files couldn't be deleted. Check the game's folder." : L"Deleted " + deletingTitle + L". Your saves were kept.");
            } else {
                notice.Text(L"Deleting " + deletingTitle + L"\u2026  " + std::to_wstring(deletedFiles.load()) + L" files");
            }
        }
        if ((devPlay || devDelete || devLogs) && devWait && !--devWait) {
            if (devLogs) { devLogs = false; tab = 0; detailsOpen = true; Refresh(); ExportLogs(); } // dev-export-logs.txt
            if (devPlay) { devPlay = false; tab = 0; Refresh(); Play(); }
            if (devDelete) { devDelete = false; AskDelete(); ConfirmDelete(); }
        }
        const auto pads = Gamepad::Gamepads();
        if (!pads.Size()) { repeatDirection = 0; return; }
        const auto reading = pads.GetAt(0).GetCurrentReading();
        uint32_t buttons = static_cast<uint32_t>(reading.Buttons);
        constexpr uint32_t left = static_cast<uint32_t>(GamepadButtons::DPadLeft), right = static_cast<uint32_t>(GamepadButtons::DPadRight);
        if (reading.LeftThumbstickX < -0.6) buttons |= left;
        if (reading.LeftThumbstickX > 0.6) buttons |= right;
        if (!padSeen) { padSeen = true; padPrevious = buttons; return; } // ignore buttons still held from the game
        const uint32_t pressed = buttons & ~padPrevious;
        padPrevious = buttons;
        auto on = [&](GamepadButtons b) { return (pressed & static_cast<uint32_t>(b)) != 0; };
        if (on(GamepadButtons::A)) { if (confirmOpen) { if (confirmKind == 5) OpenUpdate(); else if (confirmKind == 4) ConfirmImport(); else if (confirmKind == 1) ConfirmFsr(); else if (confirmKind == 2) DeleteConvertedPkgs(); else if (confirmKind == 3) ConfirmUseUp(); else ConfirmDelete(); } else if (tab == 1) CopyUsbSelected(); else Play(); }
        if (on(GamepadButtons::X)) {
            if (tab == 1) { if (!confirmOpen) ScanUsb(); }
            else if (confirmOpen && confirmKind == 4) NextImport();
            else if (detailsOpen) AskImport();
            else AskDelete();
        }
        if (on(GamepadButtons::View) && tab == 0 && detailsOpen) ExportSaves();
        if (on(GamepadButtons::RightThumbstick) && tab == 0 && detailsOpen) ExportLogs();
        if (tab == 1 && !usb.empty() && !usbScanning && !confirmOpen) {
            if (on(GamepadButtons::DPadDown) && usbSelected + 1 < usb.size()) { ++usbSelected; RefreshUsb(); }
            if (on(GamepadButtons::DPadUp) && usbSelected) { --usbSelected; RefreshUsb(); }
        }
        if (tab == 0 && on(GamepadButtons::DPadUp)) ToggleFps();
        if (tab == 0 && on(GamepadButtons::DPadDown)) {
            if (!games.empty() && selected < games.size() && HasFsrUpscale(games[selected].id)) ToggleUpscale(); else ToggleFps();
        }
        if (tab == 0 && on(GamepadButtons::Menu)) ToggleCpu();
        if (on(GamepadButtons::B)) Back();
        if (on(GamepadButtons::Y)) { if (tab == 1) MountSelected(); else Details(); }
        if (on(GamepadButtons::LeftShoulder)) SwitchTab(tab - 1);
        if (on(GamepadButtons::RightShoulder)) SwitchTab(tab + 1);
        const int direction = (buttons & left) ? -1 : (buttons & right) ? 1 : 0;
        const auto time = std::chrono::steady_clock::now();
        if (direction != repeatDirection) {
            repeatDirection = direction;
            if (direction) { Move(direction); repeatAt = time + std::chrono::milliseconds(420); }
        } else if (direction && time >= repeatAt) {
            Move(direction);
            repeatAt = time + std::chrono::milliseconds(130);
        }
    }

    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&) {
        RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
        Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
            Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
        Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().IsScreenCaptureEnabled(true);
        // Games live on the Xbox's large development drive when the app can write there (the
        // app-data drive is small and can be full); otherwise in LocalState (a PC).
        const bool isXbox = Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily() == L"Windows.Xbox";
        dataRoot = isXbox ? L"D:\\DevelopmentFiles\\XStation" : local;
        gamesDriveMissing = isXbox;
        CreateDirectoryW(L"D:\\DevelopmentFiles\\XStation", nullptr);
        if (Exists(L"D:\\DevelopmentFiles\\XStation")) {
            const std::wstring probe = L"D:\\DevelopmentFiles\\XStation\\.write-check";
            HANDLE h = CreateFile2(probe.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); DeleteFileW(probe.c_str()); dataRoot = L"D:\\DevelopmentFiles\\XStation"; gamesDriveMissing = false; }
        }
        CreateDirectoryW((dataRoot + L"\\Games").c_str(), nullptr);
        CreateDirectoryW((dataRoot + L"\\Import").c_str(), nullptr);
        Build();
        if (gamesDriveMissing) notice.Text(L"XStation can't write to D:\\DevelopmentFiles, so it can't store games right now.");
        if (RunsAsApp()) notice.Text(kSetAsGame);
        Rescan();
        ShowLastExit();
        if (MountMissing()) WakeUsb();
#ifndef XSTATION_PUBLIC
        updateTestDev = Exists(dataRoot + L"\\dev-update-test.txt"); // test switch: the box shows for any release
#endif
        CheckForUpdate();
#ifndef XSTATION_PUBLIC // the dev-*.txt test switches, none in a public build
        // Developer switches for testing with no controller; absent on a normal install.
        const std::wstring devPlayFile = dataRoot + L"\\dev-autoplay.txt", devDeleteFile = dataRoot + L"\\dev-autodelete.txt";
        const std::wstring devLogsFile = dataRoot + L"\\dev-export-logs.txt"; // 2026-11-18: <title id>: Details + copy its logs to USB
        for (const auto& [file, flag] : {std::pair<std::wstring, bool*>{devPlayFile, &devPlay}, std::pair<std::wstring, bool*>{devDeleteFile, &devDelete},
                                         std::pair<std::wstring, bool*>{devLogsFile, &devLogs}}) {
            std::wifstream dev(file);
            std::wstring wanted;
            if (dev >> wanted) {
                dev.close();
                DeleteFileW(file.c_str());
                for (size_t i = 0; i < games.size(); ++i) if (games[i].id == wanted) { selected = i; *flag = true; devWait = 90; }
            }
        }
        // Developer switch for testing Wi-Fi without a controller; absent on a normal install.
        if (Exists(dataRoot + L"\\dev-open-add-page.txt")) tab = 1;
#ifndef XSTATION_PUBLIC
        if (Exists(dataRoot + L"\\dev-open-compat-page.txt")) tab = 2; // 2026-11-18: a screenshot of the Compatible tab
#endif
        // Developer switch (2026-11-15): dev-open-details.txt opens Details on the first game (a screenshot of the panel
        // without key presses); a line "status" also shows a sample save status. Absent on a normal install.
        {
            std::wifstream dev(dataRoot + L"\\dev-open-details.txt");
            if (dev && !games.empty()) {
                std::wstring what;
                dev >> what;
                detailsOpen = true;
                if (what == L"status") SaveStatus(L"Exported 16 save files (20 MB) to the USB drive: XStation\\Saves\\" + games[0].id + L"\\2026-11-15 18-40-00", true);
                if (what == L"error") SaveStatus(L"Couldn't export the saves to the USB drive: create E:\\XStation\\Saves\\x\\savedata\\a (error 5). Your saves on the Xbox weren't changed.", false);
            }
        }
        // Developer switch: dev-prepare-test.txt, lines "<title id>|<game folder>": makes each title's images in memory
        // (nothing written) and logs their SHA-256 to xstation-usb.log; absent on a normal install.
        {
            std::ifstream request(dataRoot + L"\\dev-prepare-test.txt");
            std::vector<std::pair<std::wstring, std::wstring>> jobs;
            std::string line;
            while (std::getline(request, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                const size_t bar = line.find('|');
                if (bar != std::string::npos) jobs.push_back({FromUtf8(line.substr(0, bar)), FromUtf8(line.substr(bar + 1))});
            }
            request.close();
            if (!jobs.empty()) {
                DeleteFileW((dataRoot + L"\\dev-prepare-test.txt").c_str());
                std::thread([this, keep = get_strong(), jobs] {
                    for (const auto& [id, folder] : jobs) {
                        std::vector<uint8_t> boot, content, memory;
                        std::wstring err;
                        std::string detail;
                        const auto began = std::chrono::steady_clock::now();
                        const bool ok = GamePrepareImages(id, folder + L"\\" + GamePrepareDataFolder(id), package + L"\\Engines\\" + id, boot, content, err, detail, &memory);
                        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
                        UsbLog(L"prepare test " + id + L": " + (ok ? L"ok" : err) + L" in " + std::to_wstring(int(s * 1000)) + L" ms\n" + FromUtf8(detail));
                    }
                    UsbLog(L"prepare test done");
                }).detach();
            }
        }
        // Developer switch: open one file every way a game engine does and report it; absent on a normal install.
        {
            std::ifstream request(dataRoot + L"\\dev-usb-fopen.txt");
            std::string target;
            if (std::getline(request, target)) {
                request.close();
                DeleteFileW((dataRoot + L"\\dev-usb-fopen.txt").c_str());
                while (!target.empty() && (target.back() == '\r' || target.back() == ' ')) target.pop_back();
                std::ofstream out(dataRoot + L"\\xstation-usb-fopen.txt", std::ios::trunc | std::ios::binary);
                out << "target " << target << "\n";
                out << "fallback calls so far " << XStationUsbFilesFallbacks() << "\n";
                errno = 0;
#pragma warning(suppress : 4996) // the engines call it this way
                FILE* f = fopen(target.c_str(), "rb");
                out << "fopen: " << (f ? "opened" : "failed, errno ") << (f ? 0 : errno) << "\n";
                if (f) { char b[16]; out << "  fread " << fread(b, 1, sizeof(b), f) << "\n"; fclose(f); }
                const std::wstring wide = FromUtf8(target);
#pragma warning(suppress : 4996) // the engines call it this way
                FILE* fw = _wfopen(wide.c_str(), L"rb");
                out << "_wfopen: " << (fw ? "opened" : "failed") << "\n";
                if (fw) fclose(fw);
                std::ifstream stream(wide, std::ios::binary);
                out << "ifstream: " << (stream.is_open() ? "opened" : "failed") << "\n";
                HANDLE h = CreateFile2(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
                out << "CreateFile2: " << (h != INVALID_HANDLE_VALUE ? "opened" : "failed, error ") << (h != INVALID_HANDLE_VALUE ? 0 : GetLastError()) << "\n";
                if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
                out << "GetFileAttributesW: " << GetFileAttributesW(wide.c_str()) << "\n";
                out << "fallback calls now " << XStationUsbFilesFallbacks() << "\n";
                out << ToUtf8(XStationUsbFilesReport());
            }
        }
        // Developer switch: a USB scan (and its access check) at startup; absent on a normal install.
        const bool devUsbProbe = Exists(dataRoot + L"\\dev-usb-probe.txt");
        if (devUsbProbe) { DeleteFileW((dataRoot + L"\\dev-usb-probe.txt").c_str()); tab = 1; usbProbeWanted = true; }
        {
            std::ifstream action(dataRoot + L"\\dev-usb-action.txt");
            std::string text;
            if (std::getline(action, text)) {
                action.close();
                DeleteFileW((dataRoot + L"\\dev-usb-action.txt").c_str());
                devUsbAction = FromUtf8(text);
                while (!devUsbAction.empty() && (devUsbAction.back() == L'\r' || devUsbAction.back() == L' ')) devUsbAction.pop_back();
                tab = 1;
            }
        }
#endif
        {
            std::ofstream log(dataRoot + L"\\xstation-wifi.log", std::ios::app);
            log << "dashboard games=" << games.size() << " tab=" << tab << " memory_limit_MiB=" << (Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20)
                << " set_as_app=" << RunsAsApp() << "\n";
        }
        Refresh();

        auto window = Window::Current();
        window.CoreWindow().KeyDown([this](auto const&, Windows::UI::Core::KeyEventArgs const& e) {
            using VK = Windows::System::VirtualKey;
            switch (e.VirtualKey()) {
            case VK::Left: Move(-1); break;
            case VK::Right: Move(1); break;
            case VK::Up: ToggleFps(); break;
            case VK::Down:
                if (!games.empty() && selected < games.size() && HasFsrUpscale(games[selected].id)) ToggleUpscale(); else ToggleFps();
                break;
            case VK::Enter: if (confirmOpen) { if (confirmKind == 5) OpenUpdate(); else if (confirmKind == 4) ConfirmImport(); else if (confirmKind == 1) ConfirmFsr(); else if (confirmKind == 2) DeleteConvertedPkgs(); else if (confirmKind == 3) ConfirmUseUp(); else ConfirmDelete(); } else if (tab == 1) CopyUsbSelected(); else Play(); break;
            case VK::X: if (tab == 1) { if (!confirmOpen) ScanUsb(); } else AskDelete(); break;
            case VK::Delete: AskDelete(); break;
            case VK::C: ToggleCpu(); break;
            case VK::S: ExportSaves(); break;
            case VK::L: ExportLogs(); break;
            case VK::I: if (confirmOpen && confirmKind == 4) NextImport(); else AskImport(); break;
            case VK::Escape: Back(); break;
            case VK::Y: if (tab == 1) MountSelected(); else Details(); break;
            case VK::Q: SwitchTab(tab - 1); break;
            case VK::E: SwitchTab(tab + 1); break;
            default: break;
            }
        });
        Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
            [this](auto const&, Windows::UI::Core::BackRequestedEventArgs const& e) {
                e.Handled(true); // B is read from the pad in Poll
            });
        window.Activate();
        timer = DispatcherTimer();
        timer.Interval(std::chrono::milliseconds(33));
        timer.Tick([this](auto const&, auto const&) { Poll(); });
        timer.Start();
    }
};

} // namespace

void RunXStationDashboard(const std::wstring& localRoot, const std::wstring& packageRoot) {
    Application::Start([localRoot, packageRoot](auto const&) {
        auto app = make_self<DashApp>();
        app->local = localRoot;
        app->package = packageRoot;
        GameEnginesConfigure(localRoot); // 2026-11-17: other regions' engine choices
        app.detach();
    });
}

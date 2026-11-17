// XStation (2026-11-14): The Last of Us's Xbox assets made on the console (see tlou_assets.h). C++ ports of
// reduce_texture_margin.py (+ reduce_home_textures.py, psarc_reader.py) and tools/psarc_make_packs.py,
// psarc_area_index.py; checked byte for byte against their output on the PC (tools/pkgtest tlouassets).
// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlou_assets.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "third_party/miniz/miniz.h"

namespace {

using Bytes = std::vector<uint8_t>;
[[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why); }

constexpr char kHomeOriginal[] = "260933a8dc29769e8217025c726d9fbbfc3e77739f2be7bb72e7b3c376743601";
constexpr char kHomeReduced[] = "c1cb0e4ea9ca1c41a7ecf41a449607e2b62209b78a888d06e09f930fcae1855b";

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) < 0) fail("SHA-256 unavailable");
    }
    ~Sha256() { if (h) BCryptDestroyHash(h); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    void Add(const uint8_t* p, size_t n) {
        while (n) {
            const ULONG k = (ULONG)std::min<size_t>(n, 1u << 30);
            BCryptHashData(h, (PUCHAR)p, k, 0);
            p += k; n -= k;
        }
    }
    std::string Hex() {
        uint8_t out[32];
        BCryptFinishHash(h, out, 32, 0);
        static const char* d = "0123456789abcdef";
        std::string s;
        for (uint8_t b : out) { s += d[b >> 4]; s += d[b & 15]; }
        return s;
    }
};

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
    explicit File(const std::wstring& path, bool write = false) {
        h = CreateFile2FromAppW(path.c_str(), write ? GENERIC_WRITE : GENERIC_READ, write ? 0 : FILE_SHARE_READ,
                                write ? CREATE_ALWAYS : OPEN_EXISTING, nullptr);
        if (h == INVALID_HANDLE_VALUE) fail("can't open " + Narrow(path) + " (error " + std::to_string(GetLastError()) + ")");
        LARGE_INTEGER s{};
        if (!write && GetFileSizeEx(h, &s)) size = (uint64_t)s.QuadPart;
    }
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    File(const File&) = delete;
    void ReadAt(uint64_t at, uint8_t* p, size_t n) {
        LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)at;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) fail("seek failed");
        while (n) {
            DWORD got = 0;
            const DWORD want = (DWORD)std::min<size_t>(n, 64u << 20);
            if (!ReadFile(h, p, want, &got, nullptr) || !got) fail("read failed");
            p += got; n -= got;
        }
    }
    void Write(const uint8_t* p, size_t n) {
        while (n) {
            DWORD wrote = 0;
            const DWORD want = (DWORD)std::min<size_t>(n, 64u << 20);
            if (!WriteFile(h, p, want, &wrote, nullptr) || wrote != want) fail("write failed (drive full?)");
            p += wrote; n -= wrote;
        }
    }
};

std::string FileSha256(const std::wstring& path) {
    File f(path);
    Sha256 sha;
    Bytes buf(8u << 20);
    for (uint64_t at = 0; at < f.size;) {
        const size_t n = (size_t)std::min<uint64_t>(buf.size(), f.size - at);
        f.ReadAt(at, buf.data(), n);
        sha.Add(buf.data(), n);
        at += n;
    }
    return sha.Hex();
}

uint32_t BE32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t BE40(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 5; ++i) v = v << 8 | p[i]; return v; }

// PSARC (zlib, 30-byte entries). Two readers, as the two Python tools read: Block() as psarc_reader.py (a block
// stored at full length is raw, else zlib), Loose() as psarc_extract_shaders.entries_of (size 0 = raw block,
// else zlib when it starts with 0x78 and inflates, raw otherwise).
struct Psarc {
    File file;
    uint32_t toc = 0, stride = 0, count = 0, block = 0, zWidth = 2;
    struct Entry { uint8_t md5[16]; uint32_t first; uint64_t size, offset; };
    std::vector<Entry> entries;
    std::vector<uint32_t> zsizes;
    explicit Psarc(const std::wstring& path) : file(path) {
        uint8_t head[32];
        file.ReadAt(0, head, 32);
        if (memcmp(head, "PSAR", 4) || memcmp(head + 8, "zlib", 4)) fail("not a zlib psarc");
        toc = BE32(head + 12); stride = BE32(head + 16); count = BE32(head + 20); block = BE32(head + 24);
        if (stride != 30 || !count || count > 1000000 || toc < 32 + stride * count || toc > file.size) fail("unexpected psarc layout");
        zWidth = block <= 0x10000 ? 2 : block <= 0x1000000 ? 3 : 4;
        Bytes t(toc - 32);
        file.ReadAt(32, t.data(), t.size());
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = &t[i * stride];
            Entry en{};
            memcpy(en.md5, e, 16);
            en.first = BE32(e + 16); en.size = BE40(e + 20); en.offset = BE40(e + 25);
            entries.push_back(en);
        }
        for (size_t at = stride * count; at + zWidth <= t.size(); at += zWidth) {
            uint32_t v = 0;
            for (uint32_t k = 0; k < zWidth; ++k) v = v << 8 | t[at + k];
            zsizes.push_back(v);
        }
    }
    bool Inflate(const Bytes& in, Bytes& out, size_t limit) {
        out.resize(limit);
        const size_t got = tinfl_decompress_mem_to_mem(out.data(), limit, in.data(), in.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
        out.resize(got);
        return true;
    }
    // psarc_reader.py Psarc.block for block `i` of entry `e`: (file offset, stored, expected)
    Bytes Block(const Entry& e, uint64_t i, uint64_t& fileAt) {
        const uint64_t expected = std::min<uint64_t>(block, e.size - i * block);
        if (e.first + i >= zsizes.size()) fail("psarc block table too short");
        const uint64_t stored = zsizes[e.first + i] ? zsizes[e.first + i] : block;
        Bytes raw((size_t)stored);
        file.ReadAt(fileAt, raw.data(), raw.size());
        fileAt += stored;
        if (stored == expected) return raw;
        Bytes out;
        if (!Inflate(raw, out, (size_t)expected) || out.size() != expected) fail("bad zlib block");
        return out;
    }
    Bytes ReadAll(const Entry& e) {
        Bytes out;
        out.reserve((size_t)e.size);
        uint64_t at = e.offset;
        for (uint64_t i = 0; i * block < e.size; ++i) {
            Bytes b = Block(e, i, at);
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    }
    // psarc_extract_shaders.entries_of read(first, size, off)
    Bytes Loose(const Entry& e) {
        Bytes out;
        out.reserve((size_t)e.size);
        uint64_t at = e.offset, b = e.first;
        Bytes data, inflated;
        while (out.size() < e.size) {
            if (b >= zsizes.size()) fail("psarc block table too short");
            const uint32_t z = zsizes[b++];
            const size_t want = (size_t)std::min<uint64_t>(block, e.size - out.size());
            if (z == 0) {
                data.resize(want);
                file.ReadAt(at, data.data(), want);
                at += want;
                out.insert(out.end(), data.begin(), data.end());
                continue;
            }
            data.resize(z);
            file.ReadAt(at, data.data(), z);
            at += z;
            if (data[0] == 0x78 && Inflate(data, inflated, std::max<size_t>(block, 1u << 16))) out.insert(out.end(), inflated.begin(), inflated.end());
            else out.insert(out.end(), data.begin(), data.end());
        }
        return out;
    }
};

// Sequential reader over a PSARC entry (psarc_reader.py blocks), for the 787 MB texture dictionary.
struct EntryStream {
    Psarc& arc;
    const Psarc::Entry& e;
    uint64_t blockIndex = 0, fileAt, blockStart = 0;
    Bytes cur;
    EntryStream(Psarc& a, const Psarc::Entry& en) : arc(a), e(en), fileAt(en.offset) {}
    // bytes [at, at+n) of the entry; `at` never goes backwards
    void Read(uint64_t at, uint8_t* p, size_t n) {
        while (n) {
            while (at >= blockStart + cur.size()) {
                if (blockIndex * arc.block >= e.size) fail("read past the entry");
                blockStart = blockIndex * arc.block;
                cur = arc.Block(e, blockIndex, fileAt);
                ++blockIndex;
            }
            if (at < blockStart) fail("entry read went backwards");
            const size_t k = (size_t)std::min<uint64_t>(n, blockStart + cur.size() - at);
            memcpy(p, cur.data() + (at - blockStart), k);
            p += k; at += k; n -= k;
        }
    }
};

// ---- world-home textures (reduce_home_textures.py inspect/mip_sizes/selected + reduce_texture_margin.py policy) ----
const std::map<uint32_t, uint32_t> kBlockBytes = {{71, 8}, {72, 8}, {74, 16}, {75, 16}, {77, 16}, {78, 16},
                                                  {80, 8}, {81, 8}, {83, 16}, {84, 16}, {98, 16}, {99, 16}};
const char* kExcluded[] = {"/characters/", "/ui/", "/fonts/", "/effects/", "/fx/", "/cinematics/", "/menus/"};

struct Texture { uint32_t offset, dataOffset, size, type, format, mips, width, height; std::string path; };

uint32_t U32(const Bytes& d, uint64_t o) { if (o + 4 > d.size()) fail("metadata read out of range"); uint32_t v; memcpy(&v, &d[o], 4); return v; }
uint64_t U64(const Bytes& d, uint64_t o) { if (o + 8 > d.size()) fail("metadata read out of range"); uint64_t v; memcpy(&v, &d[o], 8); return v; }
std::string Str(const Bytes& d, uint64_t o) {
    if (o >= d.size()) fail("string out of range");
    const uint64_t end = std::min<uint64_t>(o + 4096, d.size());
    for (uint64_t i = o; i < end; ++i) if (!d[i]) return std::string((const char*)&d[o], (size_t)(i - o));
    fail("unterminated string");
}
std::string Lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return s; }
bool Pow2(uint32_t v) { return v && !(v & (v - 1)); }

std::vector<uint64_t> MipSizes(uint32_t format, uint32_t width, uint32_t height, uint32_t mips) {
    const uint64_t block = kBlockBytes.at(format);
    std::vector<uint64_t> out;
    for (uint32_t i = 0; i < mips; ++i)
        out.push_back(uint64_t((std::max(1u, width >> i) + 31) / 32) * ((std::max(1u, height >> i) + 31) / 32) * 64 * block);
    return out;
}
bool Selected(const Texture& t) {
    if (t.type != 1 || !kBlockBytes.count(t.format) || t.width != t.height || t.width < 512 || !Pow2(t.width) || t.mips < 2) return false;
    const std::string p = Lower(t.path);
    for (const char* s : kExcluded) if (p.find(s) != std::string::npos) return false;
    return true;
}
uint32_t Levels(const Texture& t) {
    if (Selected(t)) return t.width > 512 ? 2 : 1;
    const bool npc = Lower(t.path).find("/characters/npc-normal/") != std::string::npos && t.type == 1 && kBlockBytes.count(t.format) &&
                     t.width >= 512 && t.height >= 512 && Pow2(t.width) && Pow2(t.height) && t.mips >= 2;
    return npc ? 1 : 0;
}

}  // namespace

int TlouReduceHomeTextures(const std::wstring& source, const std::wstring& destination, std::wstring& error, std::string& detail) {
    try {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExFromAppW(source.c_str(), GetFileExInfoStandard, &a)) { detail += "home textures: no file\n"; return 0; }
        const std::string have = FileSha256(source);
        if (have == kHomeReduced) { detail += "home textures: already reduced\n"; return 0; }
        if (have != kHomeOriginal) { detail += "home textures: not the 1.00 original (" + have.substr(0, 8) + "), left alone\n"; return 0; }
        Psarc arc(source);
        if (arc.entries.size() != 2 || arc.block != 65536) fail("world-home-textures.psarc layout changed");
        const Bytes names = arc.ReadAll(arc.entries[0]);
        const Psarc::Entry& pak = arc.entries[1];
        EntryStream in(arc, pak);
        // metadata pages: [0, raw_start)
        Bytes head(64);
        in.Read(0, head.data(), head.size());
        const uint32_t pageAt = U32(head, 20);
        Bytes page(12);
        {
            EntryStream probe(arc, pak);
            Bytes upto((size_t)pageAt + 12);
            probe.Read(0, upto.data(), upto.size());
            memcpy(page.data(), &upto[pageAt], 12);
        }
        const uint32_t pg = U32(page, 0), pageSize = U32(page, 4), flags = U32(page, 8);
        const uint64_t rawStart = uint64_t(pg) + pageSize;
        if (flags != 0 || rawStart > pak.size) fail("texture dictionary page unexpected");
        Bytes meta((size_t)rawStart);
        EntryStream metaIn(arc, pak);
        metaIn.Read(0, meta.data(), meta.size());
        if (U32(meta, 0) != 0x10a79 || U32(meta, 16) != 1 || rawStart + U32(meta, 32) != pak.size) fail("texture dictionary header unexpected");
        uint16_t count;
        memcpy(&count, &meta[pg + 18], 2);
        std::vector<Texture> textures;
        std::map<std::string, int> kinds;
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t table = uint64_t(pg) + 20 + 16ull * i;
            const uint64_t o = uint64_t(pg) + U32(meta, table + 8);
            if (o < pg || o >= rawStart) fail("descriptor out of range");
            const std::string kind = Str(meta, pg + U64(meta, o + 8));
            ++kinds[kind];
            if (kind != "VRAM_DESC") continue;
            Texture t{(uint32_t)o, U32(meta, o + 40), U32(meta, o + 48), U32(meta, o + 56), U32(meta, o + 60), U32(meta, o + 68),
                      U32(meta, o + 72), U32(meta, o + 76), Str(meta, o + 96)};
            if (U32(meta, o + 84) != 2 || U32(meta, o + 80) != 0 || !t.size || t.mips < 1 || t.mips > 16 || t.width < 1 || t.width > 8192 ||
                t.height < 1 || t.height > 8192) fail("texture descriptor unexpected");
            textures.push_back(t);
        }
        if (kinds["VRAM_DESC"] != 2039 || kinds["TEXTURE_DICTIONARY"] != 1 || kinds["VRAM_DESC_TABLE"] != 1 || kinds["PAK_LOGIN_TABLE"] != 1 || kinds.size() != 4)
            fail("texture dictionary contents unexpected");
        std::stable_sort(textures.begin(), textures.end(), [](const Texture& a, const Texture& b) { return a.dataOffset < b.dataOffset; });
        uint64_t at = 0;
        for (const auto& t : textures) { if (t.dataOffset != at) fail("texture data has gaps"); at += t.size; }
        if (at != U32(meta, 32)) fail("texture data size mismatch");
        // new metadata and the kept byte ranges
        Bytes newMeta = meta;
        struct Keep { uint64_t from, size; };
        std::vector<Keep> keep;
        uint64_t newRaw = 0, saved = 0;
        int extra = 0, npc = 0;
        auto put = [&](uint64_t off, uint32_t v) { memcpy(&newMeta[off], &v, 4); };
        for (const auto& t : textures) {
            const uint32_t levels = Levels(t);
            std::vector<uint64_t> sizes;
            const bool blockFormat = t.type == 1 && kBlockBytes.count(t.format);
            if (blockFormat) {
                sizes = MipSizes(t.format, t.width, t.height, t.mips);
                uint64_t sum = 0; for (auto s : sizes) sum += s;
                if (sum != t.size) fail("mip sizes don't add up for " + t.path);
            }
            if (levels >= t.mips) fail("too few mips in " + t.path);
            uint64_t skip = 0;
            for (uint32_t i = 0; i < levels; ++i) skip += sizes[i];
            const uint32_t newSize = uint32_t(t.size - skip);
            uint32_t mips = t.mips, width = t.width, height = t.height;
            if (levels) {
                mips -= levels; width >>= levels; height >>= levels;
                const auto check = MipSizes(t.format, width, height, mips);
                if (!std::equal(check.begin(), check.end(), sizes.begin() + levels)) fail("mip layout changed for " + t.path);
                if (Selected(t) && t.width > 512) ++extra;
                if (!Selected(t)) ++npc;
                saved += skip;
            }
            put(t.offset + 40, (uint32_t)newRaw); put(t.offset + 48, newSize); put(t.offset + 68, mips);
            put(t.offset + 72, width); put(t.offset + 76, height);
            keep.push_back({rawStart + t.dataOffset + skip, newSize});
            newRaw += newSize;
        }
        put(32, (uint32_t)newRaw);
        if (extra != 402 || npc != 231 || saved != 512229376) fail("reduction counts differ from the tested one");
        // the archive, as reduce_texture_margin.archive()
        const uint64_t length = rawStart + newRaw;
        const uint64_t blocks = (length + 65535) / 65536;
        const uint64_t tocLen = 92 + (blocks + 2) * 2;
        const uint64_t packageOffset = (tocLen + names.size() + 4095) & ~uint64_t(4095);
        const uint64_t pad = packageOffset - tocLen - names.size();
        if (pad >= 65536 || names.size() >= 65536) fail("archive layout out of range");
        Bytes out;
        auto be16 = [&](uint64_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); };
        auto be32 = [&](uint64_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); };
        auto be40 = [&](uint64_t v) { for (int s = 32; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); };
        out.insert(out.end(), {'P', 'S', 'A', 'R'}); be16(1); be16(4); out.insert(out.end(), {'z', 'l', 'i', 'b'});
        be32(tocLen); be32(30); be32(2); be32(65536); be32(0);
        out.insert(out.end(), 16, 0); be32(0); be40(names.size()); be40(tocLen);
        out.insert(out.end(), pak.md5, pak.md5 + 16); be32(2); be40(length); be40(packageOffset);
        be16(names.size()); be16(pad);
        for (uint64_t i = 0; i + 1 < blocks; ++i) be16(0);
        be16(length % 65536);
        if (out.size() != tocLen) fail("archive table size mismatch");
        out.insert(out.end(), names.begin(), names.end());
        out.insert(out.end(), (size_t)pad, 0);
        out.insert(out.end(), newMeta.begin(), newMeta.end());
        {
            File w(destination, true);
            w.Write(out.data(), out.size());
            Bytes buf;
            for (const auto& k : keep) {
                buf.resize((size_t)k.size);
                in.Read(k.from, buf.data(), buf.size());
                w.Write(buf.data(), buf.size());
            }
        }
        const std::string result = FileSha256(destination);
        if (result != kHomeReduced) {
            DeleteFileFromAppW(destination.c_str());
            fail("the result isn't the tested file (" + result.substr(0, 8) + ")");
        }
        detail += "home textures: reduced, 512229376 bytes less, sha256 " + result.substr(0, 8) + "\n";
        return 1;
    } catch (const std::exception& e) {
        DeleteFileFromAppW(destination.c_str());
        error = L"home textures: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        detail += std::string("home textures: failed: ") + e.what() + "\n";
        return -1;
    }
}

// ---- shader packs (psarc_make_packs.py) and areas.txt (psarc_area_index.py) ----
namespace {
const char* kSkipArchives[] = {"core", "gallery1", "mp-common", "mp-levels"};
const char* kSkipExt[] = {".dds", ".tga", ".PNG", ".xvag", ".bnk", ".str", ".stm", ".txt", ".json", ".fnt", ".bmfc", ".common",
                          ".subtitles", ".subtitles-temp"};
const uint8_t kSig[] = {'O', 'r', 'b', 'S', 'h', 'd', 'r'};
const uint8_t kEndpgm[] = {0x00, 0x00, 0x81, 0xbf};

bool EndsWith(const std::string& s, const char* t) { const size_t n = strlen(t); return s.size() >= n && !memcmp(s.data() + s.size() - n, t, n); }

int64_t Find(const Bytes& d, const uint8_t* sub, size_t n, size_t from) {
    if (from > d.size()) return -1;
    auto it = std::search(d.begin() + from, d.end(), sub, sub + n);
    return it == d.end() ? -1 : int64_t(it - d.begin());
}
// Python bytes.rfind(sub, lo, hi): the last match lying wholly inside [lo, hi)
int64_t RFind(const Bytes& d, const uint8_t* sub, size_t n, size_t lo, size_t hi) {
    if (hi > d.size()) hi = d.size();
    if (hi < lo + n) return -1;
    for (size_t i = hi - n + 1; i-- > lo;) if (!memcmp(&d[i], sub, n)) return int64_t(i);
    return -1;
}

// One archive's pack rows (insertion order), as make_packs main() for `arc`.
std::vector<std::pair<uint64_t, Bytes>> PackOf(const std::wstring& path) {
    Psarc arc(path);
    Bytes namesRaw = arc.Loose(arc.entries[0]);
    std::vector<std::string> names;
    {
        std::string all((const char*)namesRaw.data(), namesRaw.size()), cur;
        for (char c : all) { if (c == '\n') { names.push_back(cur); cur.clear(); } else cur += c; }
        names.push_back(cur);
    }
    std::vector<std::pair<uint64_t, Bytes>> rows;
    std::unordered_set<uint64_t> seen;
    const size_t pairs = std::min(names.size(), arc.entries.size() - 1);
    for (size_t i = 0; i < pairs; ++i) {
        const std::string& name = names[i];
        const auto& e = arc.entries[i + 1];
        bool skip = e.size == 0 || EndsWith(name, ".json");
        for (const char* x : kSkipExt) skip = skip || EndsWith(name, x);
        if (skip) continue;
        const Bytes data = arc.Loose(e);
        int64_t pos = Find(data, kSig, 7, 0);
        while (pos >= 0) {
            if (size_t(pos) + 32 <= data.size()) {
                uint32_t bits; uint64_t h;
                memcpy(&bits, &data[pos + 8], 4);
                memcpy(&h, &data[pos + 16], 8);
                const int64_t length = bits >> 8;
                const int64_t end = RFind(data, kEndpgm, 4, (size_t)std::max<int64_t>(0, pos - 0x8000), (size_t)pos);
                const int64_t start = end + 4 - length;
                if (!seen.count(h) && end >= 0 && start >= 0 && start % 4 == 0 && length % 4 == 0) {
                    int64_t lo = std::max<int64_t>(0, start - 256);
                    lo += (start - lo) % 4;
                    const size_t nWords = size_t(start - lo) / 4;
                    std::vector<uint32_t> words(nWords);
                    if (nWords) memcpy(words.data(), &data[(size_t)lo], nWords * 4);
                    int64_t mark = -1;
                    for (int64_t k = int64_t(nWords) - 1; k >= 0; --k) if (words[(size_t)k] == 0xffffffffu) { mark = k; break; }
                    if (mark >= 0) {
                        const size_t regs = nWords - size_t(mark) - 1;
                        Bytes row(20 + regs * 4 + size_t(length));
                        const uint32_t stage = (bits >> 2) & 15, code = uint32_t(length), nregs = uint32_t(regs);
                        memcpy(&row[0], &h, 8); memcpy(&row[8], &stage, 4); memcpy(&row[12], &code, 4); memcpy(&row[16], &nregs, 4);
                        if (regs) memcpy(&row[20], &words[size_t(mark) + 1], regs * 4);
                        // data[start:start+length] (a short tail when it runs past the end, as Python slicing)
                        const size_t avail = (size_t)std::min<int64_t>(length, int64_t(data.size()) - start);
                        row.resize(20 + regs * 4 + avail);
                        memcpy(&row[20 + regs * 4], &data[(size_t)start], avail);
                        seen.insert(h);
                        rows.push_back({h, std::move(row)});
                    }
                }
            }
            pos = Find(data, kSig, 7, size_t(pos) + 7);
        }
    }
    return rows;
}

std::set<uint64_t> PackHashes(const std::wstring& path) {
    std::set<uint64_t> out;
    File f(path);
    Bytes d((size_t)f.size);
    if (!d.empty()) f.ReadAt(0, d.data(), d.size());
    if (d.size() < 12 || memcmp(d.data(), "GCNPACK2", 8)) fail("bad pack " + Narrow(path));
    uint32_t count; memcpy(&count, &d[8], 4);
    size_t at = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (at + 20 > d.size()) fail("short pack " + Narrow(path));
        uint64_t h; uint32_t code, regs;
        memcpy(&h, &d[at], 8); memcpy(&code, &d[at + 12], 4); memcpy(&regs, &d[at + 16], 4);
        at += 20 + size_t(regs) * 4 + code;
        out.insert(h);
    }
    return out;
}

std::wstring LowerW(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }
}  // namespace

bool TlouMakeShaderPacks(const std::wstring& mainFolder, const std::wstring& outFolder, std::wstring& error, std::string& detail) {
    try {
        std::vector<std::wstring> stems;
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileExFromAppW((mainFolder + L"\\*.psarc").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) fail("no archives in " + Narrow(mainFolder));
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring stem = fd.cFileName;
            stem = stem.substr(0, stem.size() - 6);
            bool skip = EndsWith(Narrow(stem), "-textures");
            for (const char* s : kSkipArchives) skip = skip || Narrow(stem) == s;
            if (!skip) stems.push_back(stem);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
        std::sort(stems.begin(), stems.end(), [](const std::wstring& a, const std::wstring& b) { return LowerW(a) < LowerW(b); });
        CreateDirectoryFromAppW(outFolder.c_str(), nullptr);
        DeleteFileFromAppW((outFolder + L"\\areas.txt").c_str());
        // up to 3 archives at once (each holds one decompressed entry at a time)
        std::atomic<size_t> next{0};
        std::mutex lock;
        std::string failure;
        std::map<std::wstring, size_t> made;
        auto worker = [&] {
            for (size_t i; (i = next++) < stems.size();) {
                {
                    std::lock_guard<std::mutex> l(lock);
                    if (!failure.empty()) return;
                }
                try {
                    const auto rows = PackOf(mainFolder + L"\\" + stems[i] + L".psarc");
                    const std::wstring dest = outFolder + L"\\" + stems[i] + L".pack";
                    if (rows.empty()) { DeleteFileFromAppW(dest.c_str()); continue; }
                    const std::wstring part = dest + L".part";
                    {
                        File w(part, true);
                        Bytes head = {'G', 'C', 'N', 'P', 'A', 'C', 'K', '2', 0, 0, 0, 0};
                        const uint32_t n = (uint32_t)rows.size();
                        memcpy(&head[8], &n, 4);
                        w.Write(head.data(), head.size());
                        for (const auto& r : rows) w.Write(r.second.data(), r.second.size());
                    }
                    DeleteFileFromAppW(dest.c_str());
                    if (!MoveFileFromAppW(part.c_str(), dest.c_str())) fail("can't rename " + Narrow(part));
                    std::lock_guard<std::mutex> l(lock);
                    made[stems[i]] = rows.size();
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> l(lock);
                    if (failure.empty()) failure = Narrow(stems[i]) + ": " + e.what();
                }
            }
        };
        std::vector<std::thread> pool;
        for (int i = 0; i < 3; ++i) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        if (!failure.empty()) fail(failure);
        for (const auto& [stem, n] : made) detail += "shader pack " + Narrow(stem) + ": " + std::to_string(n) + " shaders\n";
        // areas.txt
        std::set<uint64_t> shared;
        for (const wchar_t* s : {L"bin", L"common", L"sp-common"}) {
            const std::wstring p = outFolder + L"\\" + s + L".pack";
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExFromAppW(p.c_str(), GetFileExInfoStandard, &a)) { auto h = PackHashes(p); shared.insert(h.begin(), h.end()); }
        }
        std::vector<std::wstring> worlds;
        for (const auto& [stem, n] : made) if (stem.rfind(L"world-", 0) == 0) worlds.push_back(stem);
        std::sort(worlds.begin(), worlds.end(), [](const std::wstring& a, const std::wstring& b) { return LowerW(a) < LowerW(b); });
        std::string text;
        for (const auto& w : worlds) {
            text += Narrow(w) + " ";
            bool first = true;
            for (uint64_t h : PackHashes(outFolder + L"\\" + w + L".pack")) {
                if (shared.count(h)) continue;
                char hex[24];
                snprintf(hex, sizeof(hex), "%llx", (unsigned long long)h);
                if (!first) text += " ";
                text += hex;
                first = false;
            }
            text += "\r\n";  // as Python's write_text on Windows, which the engine has been reading
        }
        {
            File w(outFolder + L"\\areas.txt.part", true);
            w.Write((const uint8_t*)text.data(), text.size());
        }
        if (!MoveFileFromAppW((outFolder + L"\\areas.txt.part").c_str(), (outFolder + L"\\areas.txt").c_str())) fail("can't write areas.txt");
        detail += "shader packs: " + std::to_string(made.size()) + " packs and areas.txt\n";
        return true;
    } catch (const std::exception& e) {
        error = L"shader packs: " + std::wstring(e.what(), e.what() + strlen(e.what()));
        detail += std::string("shader packs: failed: ") + e.what() + "\n";
        return false;
    }
}

void TlouPrepareAssets(const std::wstring& gameFolder, std::string& detail) {
    const std::wstring main = gameFolder + L"\\GameData\\build\\ps4\\main";
    const std::wstring home = main + L"\\world-home-textures.psarc";
    std::wstring err;
    const DWORD began = GetTickCount();
    if (TlouReduceHomeTextures(home, home + L".xstation-part", err, detail) == 1) {
        DeleteFileFromAppW(home.c_str());
        if (!MoveFileFromAppW((home + L".xstation-part").c_str(), home.c_str())) detail += "home textures: rename failed\n";
        else {
            // the game's recorded size (the dashboard shows it)
            HANDLE h = CreateFile2FromAppW((gameFolder + L"\\xstation-game.txt").c_str(), GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                char buf[32] = {}; DWORD got = 0;
                ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
                const unsigned long long size = strtoull(buf, nullptr, 10);
                if (size > 512229376ull) {
                    const std::string text = std::to_string(size - 512241664ull);  // 787,361,504 -> 275,119,840 bytes
                    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
                    DWORD wrote = 0;
                    WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
                    SetEndOfFile(h);
                }
                CloseHandle(h);
            }
        }
    }
    detail += "home textures step: " + std::to_string((GetTickCount() - began) / 1000) + " s\n";
    {
        HANDLE mark = CreateFile2FromAppW((gameFolder + L"\\xstation-tlou-assets.txt").c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (mark != INVALID_HANDLE_VALUE) CloseHandle(mark);
    }
    const std::wstring packs = gameFolder + L"\\shader-packs";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExFromAppW((packs + L"\\areas.txt").c_str(), GetFileExInfoStandard, &a)) { detail += "shader packs: already there\n"; return; }
    const DWORD packBegan = GetTickCount();
    TlouMakeShaderPacks(main, packs, err, detail);
    detail += "shader packs step: " + std::to_string((GetTickCount() - packBegan) / 1000) + " s\n";
}

bool TlouAssetsNeeded(const std::wstring& gameFolder) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const std::wstring home = gameFolder + L"\\GameData\\build\\ps4\\main\\world-home-textures.psarc";
    if (!GetFileAttributesExFromAppW(home.c_str(), GetFileExInfoStandard, &a)) return false;  // not a game folder we know
    // tried from Play once already: a failed or partial run doesn't repeat on every start (a prepare always runs it)
    WIN32_FILE_ATTRIBUTE_DATA m{};
    if (GetFileAttributesExFromAppW((gameFolder + L"\\xstation-tlou-assets.txt").c_str(), GetFileExInfoStandard, &m)) return false;
    const uint64_t size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    if (size == 787361504ull) return true;  // the 1.00 original's size (the step checks its SHA-256 before changing it)
    return !GetFileAttributesExFromAppW((gameFolder + L"\\shader-packs\\areas.txt").c_str(), GetFileExInfoStandard, &a);
}

// ---- Uncharted: texture dictionaries trimmed (port of tools/uc_trim_textures.py, 2026-11-14) ----
namespace {
#include "uncharted_trim_table.inc"

bool UcSelected(const Texture& t) {
    if (t.type != 1 || !kBlockBytes.count(t.format) || t.mips < 4 || std::max(t.width, t.height) < 256 || ((t.width >> 1) & 3) ||
        ((t.height >> 1) & 3) || (t.width >> 1) < 4 || (t.height >> 1) < 4) return false;
    uint64_t sum = 0;
    for (auto s : MipSizes(t.format, t.width, t.height, t.mips)) sum += s;
    return sum == t.size;
}

// One dictionary, as uc_trim_textures.trim(); the result is kept only when its SHA-256 is `want`.
void UcTrimFile(const std::wstring& source, const std::wstring& destination, const char* want) {
    File in(source);
    Bytes head(64);
    in.ReadAt(0, head.data(), head.size());
    const uint32_t pageAt = U32(head, 20);
    Bytes page(12);
    in.ReadAt(pageAt, page.data(), 12);
    const uint32_t pg = U32(page, 0), pageSize = U32(page, 4);
    const uint64_t rawStart = uint64_t(pg) + pageSize;
    if (U32(page, 8) != 0 || rawStart > in.size) fail("dictionary page unexpected");
    Bytes meta((size_t)rawStart);
    in.ReadAt(0, meta.data(), meta.size());
    if (U32(meta, 0) != 0x10a79 || U32(meta, 16) != 1 || rawStart + U32(meta, 32) != in.size) fail("not a texture dictionary");
    uint16_t count;
    memcpy(&count, &meta[pg + 18], 2);
    std::vector<Texture> textures;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t o = uint64_t(pg) + U32(meta, uint64_t(pg) + 20 + 16ull * i + 8);
        if (o < pg || o >= rawStart) fail("descriptor out of range");
        const std::string kind = Str(meta, pg + U64(meta, o + 8));
        if (kind != "VRAM_DESC") {
            if (kind != "TEXTURE_DICTIONARY" && kind != "VRAM_DESC_TABLE" && kind != "PAK_LOGIN_TABLE") fail("unexpected record " + kind);
            continue;
        }
        Texture t{(uint32_t)o, U32(meta, o + 40), U32(meta, o + 48), U32(meta, o + 56), U32(meta, o + 60), U32(meta, o + 68),
                  U32(meta, o + 72), U32(meta, o + 76), ""};
        if (!t.size || t.mips < 1 || t.mips > 16 || t.width < 1 || t.width > 16384 || t.height < 1 || t.height > 16384) fail("descriptor unexpected");
        textures.push_back(t);
    }
    std::stable_sort(textures.begin(), textures.end(), [](const Texture& a, const Texture& b) { return a.dataOffset < b.dataOffset; });
    uint64_t at = 0;
    for (const auto& t : textures) { if (t.dataOffset != at) fail("texture data has gaps"); at += t.size; }
    if (at != U32(meta, 32)) fail("texture data size mismatch");
    Bytes newMeta = meta;
    auto put = [&](uint64_t off, uint32_t v) { memcpy(&newMeta[off], &v, 4); };
    struct Keep { uint64_t from, size; };
    std::vector<Keep> keep;
    uint64_t newRaw = 0;
    for (const auto& t : textures) {
        uint64_t skip = 0;
        uint32_t mips = t.mips, width = t.width, height = t.height;
        if (UcSelected(t)) {
            const auto sizes = MipSizes(t.format, t.width, t.height, t.mips);
            skip = sizes[0];
            mips -= 1; width >>= 1; height >>= 1;
            const auto check = MipSizes(t.format, width, height, mips);
            if (!std::equal(check.begin(), check.end(), sizes.begin() + 1)) fail("mip layout changed");
        }
        const uint32_t newSize = uint32_t(t.size - skip);
        put(t.offset + 40, (uint32_t)newRaw); put(t.offset + 48, newSize); put(t.offset + 68, mips);
        put(t.offset + 72, width); put(t.offset + 76, height);
        keep.push_back({rawStart + t.dataOffset + skip, newSize});
        newRaw += newSize;
    }
    put(32, (uint32_t)newRaw);
    {
        File w(destination, true);
        w.Write(newMeta.data(), newMeta.size());
        Bytes buf;
        for (const auto& k : keep) {
            for (uint64_t done = 0; done < k.size;) {
                const size_t n = (size_t)std::min<uint64_t>(k.size - done, 32u << 20);
                buf.resize(n);
                in.ReadAt(k.from + done, buf.data(), n);
                w.Write(buf.data(), n);
                done += n;
            }
        }
    }
    const std::string got = FileSha256(destination);
    if (got != want) fail("the result isn't the tested file (" + got.substr(0, 8) + ")");
}

uint64_t SizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &a)) return UINT64_MAX;
    return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}
std::wstring UcPath(const std::wstring& gameFolder, const char* relative) {
    std::wstring p = gameFolder + L"\\GameData\\";
    for (const char* c = relative; *c; ++c) p += *c == '/' ? L'\\' : wchar_t(*c);
    return p;
}
void WriteMark(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(h);
}
}  // namespace

int UnchartedTrimTextures(const std::wstring& gameFolder, std::string& detail) {
    int trimmedNow = 0, already = 0, other = 0, failed = 0;
    for (const auto& f : kUcTrim) {
        const std::wstring path = UcPath(gameFolder, f.relative);
        const uint64_t size = SizeOf(path);
        if (size == f.trimmedBytes) { ++already; continue; }
        if (size != f.originalBytes) { ++other; detail += std::string("uncharted trim: ") + f.relative + ": not the known original, left alone\n"; continue; }
        const std::wstring part = path + L".xstation-part";
        try {
            UcTrimFile(path, part, f.trimmedSha256);
            DeleteFileFromAppW(path.c_str());
            if (!MoveFileFromAppW(part.c_str(), path.c_str())) fail("rename failed");
            ++trimmedNow;
        } catch (const std::exception& e) {
            DeleteFileFromAppW(part.c_str());
            ++failed;
            detail += std::string("uncharted trim: ") + f.relative + ": " + e.what() + "\n";
        }
    }
    const int total = int(sizeof(kUcTrim) / sizeof(kUcTrim[0]));
    detail += "uncharted trim: " + std::to_string(trimmedNow) + " trimmed now, " + std::to_string(already) + " already, " +
              std::to_string(other) + " other, " + std::to_string(failed) + " failed, of " + std::to_string(total) + "\n";
    WriteMark(gameFolder + L"\\xstation-uncharted-assets.txt", trimmedNow + already == total ? "trimmed" : "tried");
    return trimmedNow;
}

bool UnchartedAssetsNeeded(const std::wstring& gameFolder) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExFromAppW((gameFolder + L"\\xstation-uncharted-assets.txt").c_str(), GetFileExInfoStandard, &a)) return false;
    int trimmed = 0, original = 0, total = 0;
    for (const auto& f : kUcTrim) {
        const uint64_t size = SizeOf(UcPath(gameFolder, f.relative));
        trimmed += size == f.trimmedBytes;
        original += size == f.originalBytes;
        ++total;
    }
    if (trimmed == total) { WriteMark(gameFolder + L"\\xstation-uncharted-assets.txt", "trimmed"); return false; }  // trimmed by hand before
    return original > 0;
}

bool UnchartedTrimmed(const std::wstring& gameFolder) {
    HANDLE h = CreateFile2FromAppW((gameFolder + L"\\xstation-uncharted-assets.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[16] = {};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    return !strncmp(buf, "trimmed", 7);
}

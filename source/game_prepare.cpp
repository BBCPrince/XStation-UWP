// XStation: the prepared image (GameBoot) made on the console. A C++ port of the engines' Python steps:
//   Uncharted (CUSA02343): uncharted-windows/scripts prepare.py --exe eboot.bin, link_modules.py --exe eboot.bin,
//   content_profile.py --sku full.
// Every step keeps the scripts' order (Python dict insertion order, last value wins) so the image is byte-identical; the
// result is checked against the SHA-256 the engine accepts before anything is written.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <bcrypt.h>
#include <fileapifromapp.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include "game_prepare.h"

#pragma comment(lib, "bcrypt.lib")

namespace {

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const std::string& why) { throw Fail(why); }

using Bytes = std::vector<uint8_t>;

uint16_t U16(const Bytes& d, uint64_t at) { if (at + 2 > d.size()) fail("out of bounds"); return uint16_t(d[at] | d[at + 1] << 8); }
uint32_t U32(const Bytes& d, uint64_t at) { if (at + 4 > d.size()) fail("out of bounds"); uint32_t v; memcpy(&v, &d[at], 4); return v; }
uint64_t U64(const Bytes& d, uint64_t at) { if (at + 8 > d.size()) fail("out of bounds"); uint64_t v; memcpy(&v, &d[at], 8); return v; }
void Put64(Bytes& d, uint64_t at, uint64_t v) { if (at + 8 > d.size()) fail("write out of bounds"); memcpy(&d[at], &v, 8); }
Bytes Span(const Bytes& d, uint64_t at, uint64_t n) {
    if (at > d.size() || n > d.size() - at) fail("out of bounds");
    return Bytes(d.begin() + at, d.begin() + at + n);
}

void Hash(LPCWSTR alg, const void* data, size_t n, uint8_t* out, ULONG outLen) {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&h, alg, nullptr, 0) < 0) fail("crypto unavailable");
    BCryptHash(h, nullptr, 0, (PUCHAR)data, (ULONG)n, out, outLen);
    BCryptCloseAlgorithmProvider(h, 0);
}
std::string HexSha256(const Bytes& d) {
    uint8_t h[32];
    Hash(BCRYPT_SHA256_ALGORITHM, d.data(), d.size(), h, 32);
    std::string s;
    char b[3];
    for (uint8_t c : h) { sprintf_s(b, "%02x", c); s += b; }
    return s;
}

// PS4 symbol name ID: base64(SHA1(name + salt)[:8] reversed), '=' stripped, '/' -> '-'.
std::string Nid(const std::string& name) {
    static const uint8_t salt[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1, 0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
    Bytes in(name.begin(), name.end());
    in.insert(in.end(), salt, salt + 16);
    uint8_t h[20];
    Hash(BCRYPT_SHA1_ALGORITHM, in.data(), in.size(), h, 20);
    uint8_t v[8];
    for (int i = 0; i < 8; ++i) v[i] = h[7 - i];
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (int i = 0; i < 8; i += 3) {
        const uint32_t n = uint32_t(v[i]) << 16 | (i + 1 < 8 ? uint32_t(v[i + 1]) << 8 : 0) | (i + 2 < 8 ? v[i + 2] : 0);
        out += abc[(n >> 18) & 63];
        out += abc[(n >> 12) & 63];
        if (i + 1 < 8) out += abc[(n >> 6) & 63];
        if (i + 2 < 8) out += abc[n & 63];
    }
    std::replace(out.begin(), out.end(), '/', '-');
    return out;
}

std::string EncodeId(uint64_t value) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string r(1, abc[value & 63]);
    value >>= 6;
    while (value) { r.insert(r.begin(), abc[value & 63]); value >>= 6; }
    return r;
}

Bytes ReadAll(const std::wstring& path) {
    CREATEFILE2_EXTENDED_PARAMETERS p{sizeof(p)};
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    p.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &p);
    if (h == INVALID_HANDLE_VALUE) fail("can't open " + std::string(path.begin(), path.end()));
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > (1ll << 31)) { CloseHandle(h); fail("file too large"); }
    Bytes d(size_t(size.QuadPart));
    size_t at = 0;
    while (at < d.size()) {
        DWORD got = 0;
        if (!ReadFile(h, d.data() + at, DWORD(std::min<size_t>(d.size() - at, 1u << 30)), &got, nullptr) || !got) { CloseHandle(h); fail("read failed"); }
        at += got;
    }
    CloseHandle(h);
    return d;
}

// ---- prepare.py: parse_self ----
struct Ph {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Elf {
    Bytes elf;
    uint64_t entry = 0;
    std::vector<Ph> ph;
};

void ParseHeaders(const Bytes& data, uint64_t base, uint64_t& entry, uint64_t& phoff, uint16_t& phnum, std::vector<Ph>& ph) {
    const Bytes ident = Span(data, base, 16);
    static const uint8_t want[7] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
    if (memcmp(ident.data(), want, 7) || U16(data, base + 18) != 62) fail("expected little-endian x86-64 ELF");
    entry = U64(data, base + 24);
    phoff = U64(data, base + 32);
    const uint16_t phentsize = U16(data, base + 54);
    phnum = U16(data, base + 56);
    if (phentsize != 56 || !(phnum > 0 && phnum < 256)) fail("unsupported program headers");
    for (uint16_t i = 0; i < phnum; ++i) {
        const uint64_t at = base + phoff + uint64_t(i) * 56;
        Ph p{U32(data, at), U32(data, at + 4), U64(data, at + 8), U64(data, at + 16), U64(data, at + 24), U64(data, at + 32), U64(data, at + 40), U64(data, at + 48)};
        ph.push_back(p);
    }
}

bool Required(uint32_t type) { return type == 1 || type == 2 || type == 0x61000000 || type == 0x61000010; }

Elf ParseSelf(const Bytes& data) {
    Elf e;
    uint64_t phoff = 0;
    uint16_t phnum = 0;
    const Bytes magic = Span(data, 0, 4);
    if (!memcmp(magic.data(), "\x7f" "ELF", 4)) {
        ParseHeaders(data, 0, e.entry, phoff, phnum, e.ph);
        uint64_t end = 0;
        for (const auto& p : e.ph) end = std::max(end, p.offset + p.filesz);
        if (end > 512ull * 1024 * 1024) fail("probe image exceeds 512 MiB limit");
        e.elf.assign(end, 0);
        const uint64_t headerEnd = phoff + uint64_t(phnum) * 56;
        const Bytes head = Span(data, 0, headerEnd);
        std::copy(head.begin(), head.end(), e.elf.begin());
        for (size_t i = 0; i < e.ph.size(); ++i) {
            const Ph& p = e.ph[i];
            if (!p.filesz) continue;
            if (p.offset + p.filesz > data.size()) { if (Required(p.type)) fail("required segment unavailable"); continue; }
            std::copy(data.begin() + p.offset, data.begin() + p.offset + p.filesz, e.elf.begin() + p.offset);
        }
        return e;
    }
    if (memcmp(magic.data(), "O\x15=\x1d", 4)) fail("expected PS4 SELF or ELF64");
    const uint16_t count = U16(data, 24);
    const uint64_t base = 32 + uint64_t(count) * 32;
    ParseHeaders(data, base, e.entry, phoff, phnum, e.ph);
    uint64_t end = 0;
    for (const auto& p : e.ph) end = std::max(end, p.offset + p.filesz);
    if (end > 512ull * 1024 * 1024) fail("probe image exceeds 512 MiB limit");
    e.elf.assign(end, 0);
    const uint64_t headerEnd = phoff + uint64_t(phnum) * 56;
    const Bytes head = Span(data, base, headerEnd);
    std::copy(head.begin(), head.end(), e.elf.begin());
    std::vector<std::pair<uint64_t, uint64_t>> covered{{0, headerEnd}};
    for (uint16_t i = 0; i < count; ++i) {
        const uint64_t flags = U64(data, 32 + uint64_t(i) * 32), off = U64(data, 40 + uint64_t(i) * 32), size = U64(data, 48 + uint64_t(i) * 32),
                       mem = U64(data, 56 + uint64_t(i) * 32);
        if (!(flags & 0x800)) continue;
        if (flags & 10) fail("encrypted/compressed SELF segment is unsupported");
        const uint64_t index = (flags >> 20) & 4095;
        if (index >= e.ph.size()) fail("invalid segment index");
        const Ph& p = e.ph[index];
        if (size != p.filesz || size != mem) fail("unsupported blocked segment layout");
        const Bytes seg = Span(data, off, size);
        std::copy(seg.begin(), seg.end(), e.elf.begin() + p.offset);
        covered.push_back({p.offset, p.offset + size});
    }
    for (const auto& p : e.ph) {
        if (!p.filesz) continue;
        bool ok = false;
        for (const auto& [a, z] : covered) if (a <= p.offset && p.offset + p.filesz <= z) ok = true;
        if (!ok && Required(p.type)) fail("required segment is unavailable");
    }
    return e;
}

std::string CString(const Bytes& table, uint64_t off) {
    if (off >= table.size()) fail("string out of bounds");
    const auto end = std::find(table.begin() + off, table.end(), uint8_t(0));
    if (end == table.end()) fail("unterminated string");
    return std::string(table.begin() + off, end);
}

const Ph* First(const std::vector<Ph>& ph, uint32_t type) {
    for (const auto& p : ph) if (p.type == type) return &p;
    return nullptr;
}

// param.sfo values: integers (format 0x404) and strings.
struct SfoValue {
    bool isInt = false;
    uint32_t number = 0;
    std::string text;
};
std::map<std::string, SfoValue> Sfo(const Bytes& d) {
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4)) fail("bad SFO signature");
    const uint32_t keys = U32(d, 8), values = U32(d, 12), count = U32(d, 16);
    std::map<std::string, SfoValue> out;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t at = 20 + uint64_t(i) * 16;
        const uint16_t key = U16(d, at), fmt = U16(d, at + 2);
        const uint32_t size = U32(d, at + 4), off = U32(d, at + 12);
        const std::string k = CString(d, keys + uint64_t(key));
        const Bytes v = Span(d, values + uint64_t(off), size);
        SfoValue val;
        if (fmt == 0x404) {
            if (v.size() != 4) fail("bad SFO integer");
            val.isInt = true;
            memcpy(&val.number, v.data(), 4);
        } else {
            std::string t(v.begin(), v.end());
            while (!t.empty() && t.back() == '\0') t.pop_back();
            val.text = t;
        }
        out[k] = val;
    }
    return out;
}

// ---- prepare.py: prepare (the BBPROBE2 boot, kept in memory) ----
struct Reloc {
    uint64_t target;
    uint64_t kind;
    int64_t a, b;
};
struct Seg {
    uint64_t vaddr, memsz, flags;
};
struct Boot {
    uint64_t size = 0, entry = 0, flags = 0;
    std::vector<Seg> segments;
    std::vector<std::string> names;
    std::vector<Reloc> relocs;
    Bytes image;
};

// Dynamic table entries up to the null tag, and the dict of them (later duplicates win).
std::vector<std::pair<uint64_t, uint64_t>> Dynamic(const Elf& e, bool stopAtNull) {
    const Ph* dp = First(e.ph, 2);
    if (!dp) fail("no dynamic segment");
    std::vector<std::pair<uint64_t, uint64_t>> dyn;
    for (uint64_t pos = dp->offset; pos < dp->offset + dp->filesz; pos += 16) {
        const uint64_t tag = U64(e.elf, pos), value = U64(e.elf, pos + 8);
        if (stopAtNull && tag == 0) break;
        dyn.push_back({tag, value});
    }
    return dyn;
}
uint64_t Tag(const std::vector<std::pair<uint64_t, uint64_t>>& dyn, uint64_t tag, bool* found = nullptr) {
    uint64_t v = 0;
    bool any = false;
    for (const auto& [t, val] : dyn) if (t == tag) { v = val; any = true; }
    if (found) *found = any;
    else if (!any) fail("missing dynamic tag");
    return v;
}

struct Sym {
    std::string name;
    uint32_t type, binding;
    uint16_t section;
    uint64_t value, size;
};

bool InitEnvIsRet(const std::wstring& libcPath) {
    const Elf e = ParseSelf(ReadAll(libcPath));
    const auto dyn = Dynamic(e, false); // inspect_libc: every entry of the segment
    const Ph* lib = First(e.ph, 0x61000000);
    if (!lib) fail("libc: no dynlib data");
    const Bytes blob = Span(e.elf, lib->offset, lib->filesz);
    const Bytes strings = Span(blob, Tag(dyn, 0x61000035), Tag(dyn, 0x61000037));
    const Bytes syms = Span(blob, Tag(dyn, 0x61000039), Tag(dyn, 0x6100003f));
    const std::string initEnv = Nid("_init_env"), iosInit = Nid("_ZNSt8ios_base4InitC1Ev");
    bool isRet = false;
    for (uint64_t pos = 0; pos + 24 <= syms.size(); pos += 24) {
        const uint32_t name = U32(syms, pos);
        const uint8_t info = syms[pos + 4];
        const uint16_t section = U16(syms, pos + 6);
        const uint64_t value = U64(syms, pos + 8), size = U64(syms, pos + 16);
        const std::string symbol = CString(strings, name);
        const std::string key = symbol.substr(0, symbol.find('#'));
        if ((key != initEnv && key != iosInit) || !section || (info & 15) != 2) continue;
        const Ph* code = nullptr;
        for (const auto& p : e.ph) if (p.type == 1 && (p.flags & 1) && p.vaddr <= value && value < p.vaddr + p.filesz) { code = &p; break; }
        if (!code) fail("libc: symbol outside code");
        const Bytes bytes = Span(e.elf, code->offset + value - code->vaddr, size);
        if (key == initEnv) isRet = bytes.size() == 1 && bytes[0] == 0xc3;
    }
    return isRet;
}

Boot Prepare(const std::wstring& game, const std::wstring& exe) {
    const Elf e = ParseSelf(ReadAll(game + L"\\" + exe));
    std::vector<const Ph*> loads;
    for (const auto& p : e.ph) if (p.type == 1 || p.type == 0x61000010) loads.push_back(&p);
    auto mapped = [&](uint64_t addr, uint64_t size) {
        for (const Ph* p : loads) if (p->vaddr <= addr && addr + size <= p->vaddr + p->memsz) return true;
        return false;
    };
    Boot b;
    for (const Ph* p : loads) b.size = std::max(b.size, p->vaddr + p->memsz);
    if (b.size > 512ull * 1024 * 1024 || !mapped(e.entry, 1)) fail("invalid memory image");
    b.entry = e.entry;
    b.image.assign(b.size, 0);
    for (const Ph* p : loads) {
        if (p->filesz > p->memsz) fail("segment file size exceeds memory size");
        const Bytes seg = Span(e.elf, p->offset, p->filesz);
        std::copy(seg.begin(), seg.end(), b.image.begin() + p->vaddr);
    }
    const auto dyn = Dynamic(e, true);
    const Ph* lib = First(e.ph, 0x61000000);
    if (!lib) fail("no dynlib data");
    const Bytes blob = Span(e.elf, lib->offset, lib->filesz);
    const Bytes strings = Span(blob, Tag(dyn, 0x61000035), Tag(dyn, 0x61000037));
    const Bytes syms = Span(blob, Tag(dyn, 0x61000039), Tag(dyn, 0x6100003f));
    if (syms.size() % 24) fail("bad symbol table size");
    std::vector<Sym> symbols;
    for (uint64_t pos = 0; pos < syms.size(); pos += 24) {
        const uint8_t info = syms[pos + 4];
        symbols.push_back({CString(strings, U32(syms, pos)), uint32_t(info & 15), uint32_t(info >> 4), U16(syms, pos + 6), U64(syms, pos + 8), U64(syms, pos + 16)});
    }
    std::vector<uint64_t> importOrder;
    std::map<uint64_t, size_t> imports;
    for (auto [offTag, sizeTag] : {std::pair<uint64_t, uint64_t>{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}}) {
        const Bytes table = Span(blob, Tag(dyn, offTag), Tag(dyn, sizeTag));
        if (table.size() % 24) fail("bad relocation table size");
        for (uint64_t pos = 0; pos < table.size(); pos += 24) {
            const uint64_t target = U64(table, pos), info = U64(table, pos + 8);
            const int64_t addend = (int64_t)U64(table, pos + 16);
            const uint64_t kind = info & 0xffffffff, sym = info >> 32;
            if (!mapped(target, 8)) fail("relocation outside mapped segments");
            if (kind == 8) b.relocs.push_back({target, 0, addend, 0});
            else if (kind == 1 || kind == 6 || kind == 7) {
                if (sym >= symbols.size()) fail("bad symbol index");
                const Sym& s = symbols[sym];
                if (s.section) {
                    if (s.section == 0xfff1) Put64(b.image, target, s.value + uint64_t(addend));
                    else b.relocs.push_back({target, 0, int64_t(s.value + uint64_t(addend)), 0});
                } else {
                    if (!imports.count(sym)) { imports[sym] = importOrder.size(); importOrder.push_back(sym); }
                    const uint64_t rk = s.type == 1 ? 2 : 1;
                    if ((rk == 1 && addend) || !(addend >= 0 && addend < 4096)) fail("unsupported import addend");
                    b.relocs.push_back({target, rk, int64_t(imports[sym]), addend});
                }
            } else fail("unsupported relocation " + std::to_string(kind));
        }
    }
    for (uint64_t s : importOrder) b.names.push_back(symbols[s].name);
    b.flags = InitEnvIsRet(game + L"\\sce_module\\libc.prx") ? 1 : 0;
    for (const Ph* p : loads) b.segments.push_back({p->vaddr, p->memsz, p->flags});
    return b;
}

// ---- link_modules.py ----
using LibId = std::pair<std::string, uint32_t>; // (name, version)
struct Identity {
    std::string nid;
    LibId library, module;
    bool operator<(const Identity& o) const { return std::tie(nid, library, module) < std::tie(o.nid, o.library, o.module); }
    bool operator==(const Identity& o) const { return nid == o.nid && library == o.library && module == o.module; }
};
struct MSym {
    std::string name;
    uint32_t type, binding;
    uint16_t section;
    uint64_t value, size;
    std::optional<Identity> identity;
};
struct Module {
    Elf elf;
    std::vector<std::pair<uint64_t, uint64_t>> dyn;
    std::vector<MSym> symbols;
    std::vector<std::tuple<uint64_t, uint64_t, uint64_t, int64_t>> relocs; // target, kind, symbol, addend
    std::map<std::string, LibId> libraries, modules;
    std::vector<std::string> libraryOrder, moduleOrder; // codes in first-insertion order (Python dict order)
    std::optional<Identity> IdentityOf(const std::string& name) const {
        const size_t a = name.find('#');
        if (a == std::string::npos) return std::nullopt;
        const size_t b = name.find('#', a + 1);
        if (b == std::string::npos || name.find('#', b + 1) != std::string::npos) return std::nullopt;
        auto l = libraries.find(name.substr(a + 1, b - a - 1));
        auto m = modules.find(name.substr(b + 1));
        if (l == libraries.end() || m == modules.end()) fail("unknown library or module code in " + name);
        return Identity{name.substr(0, a), l->second, m->second};
    }
};

Module LoadModule(const std::wstring& path) {
    Module m;
    m.elf = ParseSelf(ReadAll(path));
    m.dyn = Dynamic(m.elf, true);
    const Ph* lp = First(m.elf.ph, 0x61000000);
    if (!lp) fail("no dynlib data");
    const Bytes blob = Span(m.elf.elf, lp->offset, lp->filesz);
    const Bytes strings = Span(blob, Tag(m.dyn, 0x61000035), Tag(m.dyn, 0x61000037));
    for (const auto& [tag, value] : m.dyn) {
        if (tag == 0x61000013 || tag == 0x61000015 || tag == 0x6100000d || tag == 0x6100000f) {
            const bool isLibrary = tag == 0x61000013 || tag == 0x61000015;
            auto& table = isLibrary ? m.libraries : m.modules;
            const std::string key = EncodeId(value >> 48);
            const LibId id{CString(strings, value & 0xffffffff), uint32_t((value >> 32) & 65535)};
            auto it = table.find(key);
            if (it != table.end() && it->second != id) fail("conflicting module/library IDs");
            if (it == table.end()) (isLibrary ? m.libraryOrder : m.moduleOrder).push_back(key);
            table[key] = id;
        }
    }
    const Bytes table = Span(blob, Tag(m.dyn, 0x61000039), Tag(m.dyn, 0x6100003f));
    for (uint64_t pos = 0; pos + 24 <= table.size(); pos += 24) {
        const uint8_t info = table[pos + 4];
        MSym s{CString(strings, U32(table, pos)), uint32_t(info & 15), uint32_t(info >> 4), U16(table, pos + 6), U64(table, pos + 8), U64(table, pos + 16), std::nullopt};
        if (!s.name.empty()) s.identity = m.IdentityOf(s.name);
        m.symbols.push_back(s);
    }
    for (auto [ot, st] : {std::pair<uint64_t, uint64_t>{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}}) {
        const Bytes rel = Span(blob, Tag(m.dyn, ot), Tag(m.dyn, st));
        for (uint64_t pos = 0; pos + 24 <= rel.size(); pos += 24) {
            const uint64_t info = U64(rel, pos + 8);
            m.relocs.push_back({U64(rel, pos), info & 0xffffffff, info >> 32, (int64_t)U64(rel, pos + 16)});
        }
    }
    return m;
}

struct Spelling {
    const char* library;
    char lib, mod;
};
const Spelling kSpelling[] = {
    {"libSceAjm", 'N', 'O'}, {"libSceAppContent", 'c', 'd'}, {"libSceAudioIn", 'Y', 'Z'}, {"libSceAudioOut", 'G', 'H'},
    {"libSceAvPlayer", 'm', 'n'}, {"libSceCommonDialog", 'Z', 'a'}, {"libSceFios2", 'F', 'G'}, {"libSceGameLiveStreaming", 'e', 'f'},
    {"libSceGnmDriver", 'B', 'C'}, {"libSceHttp", 'K', 'L'}, {"libSceImeDialog", 'b', 'c'}, {"libSceMouse", 'U', 'V'},
    {"libSceMsgDialog", 'j', 'k'}, {"libSceNet", 'E', 'F'}, {"libSceNetCtl", 'J', 'K'}, {"libSceNpAuth", 'X', 'Y'},
    {"libSceNpCommerce", 'k', 'l'}, {"libSceNpCommon", 'Q', 'R'}, {"libSceNpManager", 'R', 'S'}, {"libSceNpMatching2", 'P', 'Q'},
    {"libSceNpProfileDialog", 'h', 'i'}, {"libSceNpScore", 'o', 'p'}, {"libSceNpSignaling", 'T', 'U'}, {"libSceNpTrophy", 'l', 'm'},
    {"libSceNpUtility", 'S', 'T'}, {"libSceNpWebApi", 'W', 'X'}, {"libScePad", 'A', 'B'}, {"libScePlayGo", 'n', 'o'},
    {"libScePosix", 'I', 'J'}, {"libSceRtc", 'H', 'I'}, {"libSceSaveData", 'O', 'P'}, {"libSceSaveDataDialog", 'd', 'e'},
    {"libSceScreenShot", 'g', 'h'}, {"libSceSsl", 'L', 'M'}, {"libSceSysmodule", 'M', 'N'}, {"libSceSystemService", 'V', 'W'},
    {"libSceUserService", 'D', 'E'}, {"libSceVideoOut", 'C', 'D'}, {"libSceVideoRecording", 'f', 'g'}, {"libSceVoice", 'i', 'j'},
    {"libSceWebBrowserDialog", 'a', 'b'}, {"libc", 'q', 'q'}, {"libkernel", 'p', 'J'},
};

std::string RuntimeName(const Identity& id) {
    for (const auto& s : kSpelling) {
        if (id.library.first != s.library) continue;
        std::string module = id.library.first;
        if (module == "libSceAppContent") module = "libSceAppContentUtil";
        else if (module == "libScePosix") module = "libkernel";
        if (id.module.first == module) return id.nid + "#" + s.lib + "#" + s.mod;
        break;
    }
    return id.nid + "#" + id.library.first;
}

void PatchFsLoads(Bytes& image, const std::vector<Ph>& ph, uint64_t base) {
    static const uint8_t fsLoad[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}; // mov rax, fs:[0]
    for (const auto& p : ph) {
        if (p.type != 1 || !(p.flags & 1)) continue;
        const uint64_t start = base + p.vaddr, end = std::min<uint64_t>(base + p.vaddr + p.filesz, image.size());
        if (start >= end) continue;
        auto it = image.begin() + start;
        while (true) {
            it = std::search(it, image.begin() + end, fsLoad, fsLoad + 9);
            if (it == image.begin() + end) break;
            *it = 0x65;
            it += 9;
        }
    }
}

// Red-zone sites (the engines' patches/redzone.json without game bytes: offset, instruction lengths, 8 bytes of
// SHA-256 of the original bytes; tools/make_redzone_sites.py).
struct RedZoneSite {
    uint64_t offset;
    std::vector<int8_t> lengths;
    uint8_t hash[8];
};
std::vector<RedZoneSite> LoadRedZoneSites(const std::wstring& path) {
    const Bytes raw = ReadAll(path);
    if (raw.size() < 44 || memcmp(raw.data(), "XSRZ0002", 8)) fail("bad red-zone site file");
    const uint32_t count = U32(raw, 8);
    std::vector<RedZoneSite> sites;
    uint64_t at = 44;
    for (uint32_t i = 0; i < count; ++i) {
        RedZoneSite s{};
        s.offset = U64(raw, at);
        const uint16_t n = U16(raw, at + 8);
        at += 10;
        for (uint16_t k = 0; k < n; ++k) s.lengths.push_back((int8_t)raw.at(at + k));
        at += n;
        const Bytes h = Span(raw, at, 8);
        memcpy(s.hash, h.data(), 8);
        at += 8;
        sites.push_back(std::move(s));
    }
    return sites;
}

// protect_image with cpu_compat off (the image equals the untranslated one, so every site is clear).
void ProtectRedZones(Bytes& image, std::vector<Seg>& segments, const std::vector<RedZoneSite>& sites, const std::vector<Reloc>& relocs, uint64_t& applied) {
    static const uint8_t leaDown[5] = {0x48, 0x8d, 0x64, 0x24, 0x80};             // lea rsp, [rsp-0x80]
    static const uint8_t leaUp[8] = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}; // lea rsp, [rsp+0x80]
    std::vector<uint64_t> skip;
    for (const auto& r : relocs) skip.push_back(r.target);
    std::sort(skip.begin(), skip.end());
    const uint64_t base = (image.size() + 65535) & ~uint64_t(65535);
    Bytes code;
    applied = 0;
    for (const auto& s : sites) {
        uint64_t len = 0;
        for (int8_t n : s.lengths) len += uint64_t(n < 0 ? -n : n);
        const uint64_t end = s.offset + len;
        if (end > image.size() || len < 5) continue;
        uint8_t h[32];
        Hash(BCRYPT_SHA256_ALGORITHM, &image[s.offset], len, h, 32);
        // bisect_right(skip, offset - 8): the first relocation target above offset - 8
        const auto nextReloc = s.offset >= 8 ? std::upper_bound(skip.begin(), skip.end(), s.offset - 8) : skip.begin();
        if (memcmp(h, s.hash, 8) || (nextReloc != skip.end() && *nextReloc < end)) continue;
        const uint64_t here = base + code.size();
        uint64_t pos = s.offset;
        for (int8_t n : s.lengths) {
            const uint64_t a = uint64_t(n < 0 ? -n : n);
            if (n < 0) code.insert(code.end(), leaDown, leaDown + 5);
            code.insert(code.end(), image.begin() + pos, image.begin() + pos + a);
            if (n < 0) code.insert(code.end(), leaUp, leaUp + 8);
            pos += a;
        }
        const int32_t back = int32_t(int64_t(end) - int64_t(base + code.size() + 5));
        code.push_back(0xe9);
        code.insert(code.end(), reinterpret_cast<const uint8_t*>(&back), reinterpret_cast<const uint8_t*>(&back) + 4);
        while (code.size() % 16) code.push_back(0xcc);
        const int32_t jump = int32_t(int64_t(here) - int64_t(s.offset + 5));
        image[s.offset] = 0xe9;
        memcpy(&image[s.offset + 1], &jump, 4);
        for (uint64_t k = s.offset + 5; k < end; ++k) image[k] = 0xcc;
        ++applied;
    }
    if (!code.empty()) {
        const uint64_t size = (code.size() + 4095) & ~uint64_t(4095);
        image.resize(base, 0);
        image.insert(image.end(), code.begin(), code.end());
        image.resize(base + size, 0xcc);
        segments.push_back({base, size, 5});
    }
}

void Append64(Bytes& out, uint64_t v) { const size_t at = out.size(); out.resize(at + 8); memcpy(&out[at], &v, 8); }

struct LinkOptions {
    bool runtimeSpelling = true;                 // Uncharted: imports spelled for the runtime's tables; God of War: as the eboot spells them
    const std::vector<RedZoneSite>* redZones = nullptr;
};

Bytes LinkModules(const std::wstring& game, const std::wstring& exe, Boot boot, const std::vector<std::wstring>& moduleNames,
                  const LinkOptions& options, std::string& detail) {
    const Module main = LoadModule(game + L"\\" + exe);
    Bytes& image = boot.image;
    // identities: insertion order of first appearance, value of the last (a Python dict built from the boot's names).
    std::vector<std::optional<Identity>> order;
    std::map<std::optional<Identity>, uint64_t> identities;
    std::vector<std::string> names;
    for (size_t i = 0; i < boot.names.size(); ++i) {
        const auto id = main.IdentityOf(boot.names[i]);
        if (!identities.count(id)) order.push_back(id);
        identities[id] = i;
        names.push_back(id && options.runtimeSpelling ? RuntimeName(*id) : boot.names[i]);
    }
    // God of War: a module's new import takes the main program's codes for its library and module when it has them.
    std::map<LibId, std::string> mainLibraries, mainModules;
    for (const auto& key : main.libraryOrder) mainLibraries[main.libraries.at(key)] = key;
    for (const auto& key : main.moduleOrder) mainModules[main.modules.at(key)] = key;
    auto imported = [&](const MSym& s) -> uint64_t {
        const auto& key = s.identity;
        if (!key) fail("import without an identity");
        if (!identities.count(key)) {
            order.push_back(key);
            identities[key] = names.size();
            if (options.runtimeSpelling) names.push_back(RuntimeName(*key));
            else {
                auto l = mainLibraries.find(key->library);
                auto m = mainModules.find(key->module);
                names.push_back(l != mainLibraries.end() && m != mainModules.end() ? key->nid + "#" + l->second + "#" + m->second
                                                                                   : key->nid + "#" + key->library.first);
            }
        }
        return identities[key];
    };
    struct Export { uint64_t address, kind; };
    std::map<Identity, Export> exports;
    std::map<std::pair<std::string, std::string>, std::vector<Export>> byNid;
    struct TableEntry { uint64_t base, size, init, tlsAddress, tlsMemsz, tlsFilesz, tlsModule; };
    std::vector<TableEntry> table;
    uint64_t base = (boot.size + 65535) & ~uint64_t(65535);
    PatchFsLoads(image, main.elf.ph, 0);
    uint64_t tlsModule = 2;
    for (const auto& file : moduleNames) {
        const Module m = LoadModule(game + L"\\sce_module\\" + file);
        std::vector<const Ph*> loads;
        for (const auto& p : m.elf.ph) if (p.type == 1 || p.type == 0x61000010) loads.push_back(&p);
        uint64_t modsize = 0;
        for (const Ph* p : loads) modsize = std::max(modsize, p->vaddr + p->memsz);
        if (base + modsize > 512ull * 1024 * 1024) fail("linked image exceeds probe limit");
        if (base + modsize > image.size()) image.resize(base + modsize, 0);
        auto mapped = [&](uint64_t addr, uint64_t len) {
            for (const Ph* p : loads) if (p->vaddr <= addr && addr + len <= p->vaddr + p->memsz) return true;
            return false;
        };
        for (const Ph* p : loads) {
            if (p->filesz > p->memsz) fail("invalid module segment size");
            const Bytes seg = Span(m.elf.elf, p->offset, p->filesz);
            std::copy(seg.begin(), seg.end(), image.begin() + base + p->vaddr);
            boot.segments.push_back({base + p->vaddr, p->memsz, p->flags});
        }
        const Ph* tls = First(m.elf.ph, 7);
        uint64_t moduleId = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !mapped(tls->vaddr, tls->memsz)) fail("unsupported module TLS layout");
            moduleId = tlsModule++;
        }
        for (auto [target, kind, symid, addend] : m.relocs) {
            if (!mapped(target, 8)) fail("module relocation outside image");
            target += base;
            if (kind == 8) boot.relocs.push_back({target, 0, int64_t(base + uint64_t(addend)), 0});
            else if (kind == 16) {
                const MSym& s = m.symbols.at(symid);
                if (!s.name.empty() || s.type != 3 || addend || !moduleId) fail("unsupported external TLS relocation");
                Put64(image, target, moduleId);
            } else if (kind == 1 || kind == 6 || kind == 7) {
                const MSym& s = m.symbols.at(symid);
                if (s.section) {
                    if (s.section == 0xfff1) Put64(image, target, s.value + uint64_t(addend));
                    else boot.relocs.push_back({target, 0, int64_t(base + s.value + uint64_t(addend)), 0});
                } else if (!s.identity && s.binding == 2) {
                    Put64(image, target, 0);
                } else {
                    const uint64_t rk = s.type == 1 ? 2 : 1;
                    if ((rk == 1 && addend) || !(addend >= 0 && addend < 4096)) fail("unsupported module import addend");
                    boot.relocs.push_back({target, rk, int64_t(imported(s)), addend});
                }
            } else fail("unsupported module relocation type");
        }
        for (const auto& s : m.symbols) {
            if (s.section && (s.binding == 1 || s.binding == 2) && s.identity && (s.type == 1 || s.type == 2)) {
                if (!mapped(s.value, std::max<uint64_t>(1, s.size))) fail("export outside load segments");
                if (exports.count(*s.identity)) fail("ambiguous export");
                const Export ex{base + s.value, s.type == 1 ? 2u : 1u};
                exports[*s.identity] = ex;
                byNid[{s.identity->nid, s.identity->library.first}].push_back(ex);
            }
        }
        PatchFsLoads(image, m.elf.ph, base);
        bool hasInit = false;
        const uint64_t init = Tag(m.dyn, 12, &hasInit);
        table.push_back({base, modsize, base + (hasInit ? init : 0), tls ? base + tls->vaddr : 0, tls ? tls->memsz : 0, tls ? tls->filesz : 0, moduleId});
        base = (base + modsize + 65535) & ~uint64_t(65535);
    }
    std::vector<std::array<uint64_t, 3>> bindings;
    for (const auto& key : order) {
        const uint64_t index = identities[key];
        std::optional<Export> found;
        if (key) {
            auto it = exports.find(*key);
            if (it != exports.end()) found = it->second;
            if (!found && key->library.first == "libSceLibcInternal") {
                auto c = byNid.find({key->nid, "libc"});
                if (c != byNid.end() && c->second.size() == 1) found = c->second[0];
            }
        }
        if (found) bindings.push_back({index, found->address, found->kind});
    }
    const Ph* mainTls = First(main.elf.ph, 7);
    uint64_t tlsValues[4] = {0, 0, 0, 0};
    if (mainTls) {
        tlsValues[0] = mainTls->vaddr; tlsValues[1] = mainTls->filesz; tlsValues[2] = mainTls->memsz; tlsValues[3] = mainTls->align;
        if (mainTls->filesz > mainTls->memsz || mainTls->memsz > 1024 * 1024 || mainTls->vaddr + mainTls->filesz > boot.size) fail("unsupported eboot TLS layout");
    }
    const Ph* procparam = First(main.elf.ph, 0x61000001);
    if (!procparam) fail("no process parameters");
    if (options.redZones) {
        uint64_t applied = 0;
        ProtectRedZones(image, boot.segments, *options.redZones, boot.relocs, applied);
        detail += "red zone: " + std::to_string(applied) + "/" + std::to_string(options.redZones->size()) + " sites\n";
    }
    Bytes out;
    const char magic[8] = {'B', 'B', 'P', 'R', 'O', 'B', 'E', '5'};
    out.insert(out.end(), magic, magic + 8);
    for (uint64_t v : {uint64_t(image.size()), boot.entry, uint64_t(boot.segments.size()), uint64_t(boot.relocs.size()), uint64_t(names.size()), boot.flags}) Append64(out, v);
    Append64(out, procparam->vaddr);
    for (uint64_t v : tlsValues) Append64(out, v);
    Append64(out, table.size());
    for (const auto& t : table) for (uint64_t v : {t.base, t.size, t.init, t.tlsAddress, t.tlsMemsz, t.tlsFilesz, t.tlsModule}) Append64(out, v);
    Append64(out, bindings.size());
    for (const auto& b : bindings) for (uint64_t v : b) Append64(out, v);
    for (const auto& s : boot.segments) for (uint64_t v : {s.vaddr, s.memsz, s.flags}) Append64(out, v);
    for (const auto& n : names) {
        if (n.size() >= 128) fail("name too long");
        const size_t at = out.size();
        out.resize(at + 128, 0);
        memcpy(&out[at], n.data(), n.size());
    }
    for (const auto& r : boot.relocs) for (uint64_t v : {r.target, r.kind, uint64_t(r.a), uint64_t(r.b)}) Append64(out, v);
    out.insert(out.end(), image.begin(), image.end());
    return out;
}

// ---- content_profile.py ----
Bytes ContentProfile(const std::wstring& game, const std::string& sku, bool titleFallback) {
    const auto values = Sfo(ReadAll(game + L"\\sce_sys\\param.sfo"));
    uint32_t params[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        auto it = values.find("USER_DEFINED_PARAM_" + std::to_string(i + 1));
        if (it != values.end()) {
            if (!it->second.isInt) fail("invalid user-defined parameter");
            params[i] = it->second.number;
        }
    }
    if (titleFallback && !values.count("USER_DEFINED_PARAM_1")) {
        auto it = values.find("TITLE_ID");
        std::string digits;
        if (it != values.end()) for (char c : it->second.text) if (c >= '0' && c <= '9') digits += c;
        params[0] = digits.empty() ? 0 : (uint32_t)std::stoul(digits);
    }
    Bytes out = {'B', 'B', 'C', 'O', 'N', 'T', '0', '1'};
    const uint32_t skuValue = sku == "full" ? 3 : 1;
    out.resize(8 + 20);
    memcpy(&out[8], &skuValue, 4);
    memcpy(&out[12], params, 16);
    return out;
}

struct Recipe {
    const wchar_t* title;
    const char* sha256;        // boot-linked.bin the engine accepts
    bool runtimeSpelling;      // link_modules.py spelling (Uncharted) or the eboot's own (God of War)
    bool titleFallback;        // content_profile.py: USER_DEFINED_PARAM_1 from the title number when missing (Uncharted)
    const wchar_t* redZones;   // site file in the engine folder, or nullptr
    uint64_t patchAt = 0;      // a 4-byte change of the finished boot-linked.bin (file offset), checked against `patchFrom`
    uint32_t patchFrom = 0, patchTo = 0;
    const wchar_t* dataFolder = L"GameData";
    const wchar_t* bootFolder = L"GameBoot";
    const wchar_t* guardSites = nullptr; // Bloodborne: a second link with these red-zone sites = boot-memory.bin
    const char* guardSha256 = nullptr;
    bool copyPatches = false;            // Bloodborne: patches.bin, patches-540p.bin from the engine folder
};
const char* kBloodbornePatchesSha256 = "3bcde5d4485ba7b129550243740d3c377572669c6c7e8a6aa9d7d9ca2db7cd6f";
const Recipe kRecipes[] = {
    {L"CUSA02343", "f05237186c2b4d25ca23bc1cc1ab7301d19efcba3a4e35b2664331479f20107c", true, true, nullptr},  // Uncharted: The Nathan Drake Collection
    {L"CUSA07408", "5b5622f78e551a88c48b2367e81a98edbc0de7478c0ac959c1ba97160dccd625", false, false, L"redzone-sites.bin"}, // God of War 1.35
    // The Last of Us Remastered 1.00 (prepare_guarded_tlou.py): its GPU wait's 2.0 s limit compares against 60.0 instead.
    {L"CUSA00552", "783f6955f13831ae0f04f350dbed99d782239e91309efcb3a577c05737cdc439", false, false, L"redzone-sites.bin",
     0xbf28c6, 0x0079e4b6, 0x0077769a},
    // Bloodborne 1.09
    {L"CUSA00900", "b5cb1da3a15bb853c2adbc9ef03d7d8058f729b247a0646370cf61f056398e1d", false, false, nullptr, 0, 0, 0,
     L"GameData109", L"GameBoot109", L"redzone-memory-sites.bin", "606d17d1a82cab5a07cff3c3e89d81b588229b4ebf069a9d07720b5445834352", true},
};
const Recipe* FindRecipe(const std::wstring& title) {
    for (const auto& r : kRecipes) if (title == r.title) return &r;
    return nullptr;
}

bool WriteAll(const std::wstring& path, const Bytes& data) {
    const std::wstring part = path + L".part";
    HANDLE h = CreateFile2FromAppW(part.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t at = 0;
    bool ok = true;
    while (ok && at < data.size()) {
        DWORD wrote = 0;
        ok = WriteFile(h, data.data() + at, DWORD(std::min<size_t>(data.size() - at, 1u << 30)), &wrote, nullptr) && wrote;
        at += wrote;
    }
    CloseHandle(h);
    if (ok) DeleteFileFromAppW(path.c_str());
    if (!ok || !MoveFileFromAppW(part.c_str(), path.c_str())) { DeleteFileFromAppW(part.c_str()); return false; }
    return true;
}

} // namespace

bool GamePrepareSupported(const std::wstring& titleId) { return FindRecipe(titleId) != nullptr; }

std::wstring GamePrepareDataFolder(const std::wstring& titleId) {
    const Recipe* r = FindRecipe(titleId);
    return r ? r->dataFolder : L"GameData";
}

bool GamePrepareImages(const std::wstring& titleId, const std::wstring& gameData, const std::wstring& engineDir, std::vector<uint8_t>& bootLinked,
                       std::vector<uint8_t>& content, std::wstring& error, std::string& detail, std::vector<uint8_t>* bootMemory) {
    const Recipe* recipe = FindRecipe(titleId);
    if (!recipe) { error = L"XStation can't prepare this game on the console yet."; return false; }
    try {
        Boot boot = Prepare(gameData, L"eboot.bin");
        detail += "prepare: image " + std::to_string(boot.size) + " bytes, " + std::to_string(boot.names.size()) + " imports, " +
                  std::to_string(boot.relocs.size()) + " relocations, init_env_is_ret " + std::to_string(boot.flags) + "\n";
        LinkOptions options;
        options.runtimeSpelling = recipe->runtimeSpelling;
        std::vector<RedZoneSite> sites;
        if (recipe->redZones) {
            sites = LoadRedZoneSites(engineDir + L"\\" + recipe->redZones);
            options.redZones = &sites;
        }
        Boot guardBoot;
        if (recipe->guardSites && bootMemory) guardBoot = boot;
        bootLinked = LinkModules(gameData, L"eboot.bin", std::move(boot), {L"libc.prx", L"libSceFios2.prx"}, options, detail);
        if (recipe->guardSites && bootMemory) {
            const std::vector<RedZoneSite> guard = LoadRedZoneSites(engineDir + L"\\" + recipe->guardSites);
            LinkOptions guarded = options;
            guarded.redZones = &guard;
            *bootMemory = LinkModules(gameData, L"eboot.bin", std::move(guardBoot), {L"libc.prx", L"libSceFios2.prx"}, guarded, detail);
            detail += "boot-memory.bin " + std::to_string(bootMemory->size()) + " bytes, sha256 " + HexSha256(*bootMemory) + "\n";
        }
        content = ContentProfile(gameData, "full", recipe->titleFallback);
        if (recipe->patchAt) {
            if (recipe->patchAt + 4 > bootLinked.size() || U32(bootLinked, recipe->patchAt) != recipe->patchFrom) fail("the image's patch site doesn't match");
            memcpy(&bootLinked[recipe->patchAt], &recipe->patchTo, 4);
            detail += "patched 4 bytes at " + std::to_string(recipe->patchAt) + "\n";
        }
        detail += "boot-linked.bin " + std::to_string(bootLinked.size()) + " bytes, sha256 " + HexSha256(bootLinked) + "\n";
    } catch (const std::exception& e) {
        const std::string what = e.what();
        error = L"Preparing failed: " + std::wstring(what.begin(), what.end());
        return false;
    }
    return true;
}

bool GamePrepare(const std::wstring& titleId, const std::wstring& gameFolder, const std::wstring& engineDir, std::wstring& error, std::string& detail) {
    const Recipe* recipe = FindRecipe(titleId);
    if (!recipe) { error = L"XStation can't prepare this game on the console yet."; return false; }
    std::vector<uint8_t> boot, content, memory;
    if (!GamePrepareImages(titleId, gameFolder + L"\\" + recipe->dataFolder, engineDir, boot, content, error, detail, &memory)) return false;
    if (HexSha256(boot) != recipe->sha256 || (recipe->guardSites && HexSha256(memory) != recipe->guardSha256)) {
        error = L"This game's version doesn't match the one its engine was built for, so it can't run yet.";
        return false;
    }
    std::vector<std::pair<std::wstring, Bytes>> patches;
    if (recipe->copyPatches) {
        try {
            for (const wchar_t* name : {L"patches.bin", L"patches-540p.bin"}) {
                Bytes data = ReadAll(engineDir + L"\\" + name);
                if (HexSha256(data) != kBloodbornePatchesSha256) { error = L"The engine's patch files changed."; return false; }
                patches.push_back({name, std::move(data)});
            }
        } catch (const std::exception&) {
            error = L"The engine folder is missing its patch files.";
            return false;
        }
    }
    const std::wstring out = gameFolder + L"\\" + recipe->bootFolder;
    CreateDirectoryFromAppW(out.c_str(), nullptr);
    bool ok = WriteAll(out + L"\\content.bin", content);
    if (recipe->guardSites) ok = ok && WriteAll(out + L"\\boot-memory.bin", memory);
    for (const auto& [name, data] : patches) ok = ok && WriteAll(out + L"\\" + name, data);
    ok = ok && WriteAll(out + L"\\boot-linked.bin", boot); // last: the dashboard treats this file as "prepared"
    if (!ok) { error = L"Couldn't write the prepared image."; return false; }
    return true;
}

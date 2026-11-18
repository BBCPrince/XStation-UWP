// XStation PKG extraction: a C++ port of OrbisPkgTool's PkgReader.ExtractAll (OrbisPkgTool/PkgReader.cs, Pfs/PfsReader.cs,
// Pfs/PFSCStream.cs, Crypto/PkgCrypto.cs), which the PC import tool runs, plus the import tool's merge of several PKGs.
// Crypto through BCrypt (in every Windows, the Xbox included); inflate through miniz's tinfl.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <bcrypt.h>
#include <fileapifromapp.h>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <cwctype>
#include <fstream>
#include <thread>
#include <cstdio>
#include <functional>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include "pkg_extract.h"
#include "pkg_keys.h"
#include "third_party/miniz/miniz.h"

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr uint64_t kBlock = 0x10000;
constexpr size_t kSector = 0x1000;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | uint64_t(Le32(p + 4)) << 32; }

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// ---- crypto (BCrypt) ----
struct Algorithms {
    BCRYPT_ALG_HANDLE sha = nullptr, hmac = nullptr, cbc = nullptr, ecb = nullptr, rsa = nullptr;
    bool ok = false;
    Algorithms() {
        ok = BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
             BCryptOpenAlgorithmProvider(&hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) >= 0 &&
             BCryptOpenAlgorithmProvider(&cbc, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
             BCryptSetProperty(cbc, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) >= 0 &&
             BCryptOpenAlgorithmProvider(&ecb, BCRYPT_AES_ALGORITHM, nullptr, 0) >= 0 &&
             BCryptSetProperty(ecb, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) >= 0 &&
             BCryptOpenAlgorithmProvider(&rsa, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0;
    }
};
Algorithms& Alg() { static Algorithms a; return a; }

void Sha256(const void* data, size_t n, uint8_t out[32]) {
    BCryptHash(Alg().sha, nullptr, 0, (PUCHAR)data, (ULONG)n, out, 32);
}
void HmacSha256(const uint8_t* key, size_t keyLen, const void* data, size_t n, uint8_t out[32]) {
    BCryptHash(Alg().hmac, (PUCHAR)key, (ULONG)keyLen, (PUCHAR)data, (ULONG)n, out, 32);
}

struct AesKey {
    BCRYPT_KEY_HANDLE h = nullptr;
    AesKey() = default;
    AesKey(BCRYPT_ALG_HANDLE alg, const uint8_t key[16]) { BCryptGenerateSymmetricKey(alg, &h, nullptr, 0, (PUCHAR)key, 16, 0); }
    AesKey(const AesKey&) = delete;
    AesKey& operator=(const AesKey&) = delete;
    AesKey(AesKey&& o) noexcept : h(o.h) { o.h = nullptr; }
    AesKey& operator=(AesKey&& o) noexcept { std::swap(h, o.h); return *this; }
    ~AesKey() { if (h) BCryptDestroyKey(h); }
};

// AES-128-CBC decrypt in place (len a multiple of 16); iv is updated to continue a stream.
bool AesCbcDecrypt(const uint8_t key[16], uint8_t iv[16], uint8_t* data, size_t len) {
    AesKey k(Alg().cbc, key);
    if (!k.h) return false;
    uint8_t next[16];
    if (len >= 16) memcpy(next, data + len - 16, 16);
    ULONG got = 0;
    uint8_t ivCopy[16];
    memcpy(ivCopy, iv, 16);
    if (BCryptDecrypt(k.h, data, (ULONG)len, nullptr, ivCopy, 16, data, (ULONG)len, &got, 0) < 0) return false;
    if (len >= 16) memcpy(iv, next, 16);
    return true;
}

// RSA-2048 PKCS#1 v1.5 decrypt with a full private key (the arrays of pkg_keys.h).
bool RsaDecrypt(const uint8_t* modulus, const uint8_t* d, const uint8_t* p, const uint8_t* q, const uint8_t* dp, const uint8_t* dq,
                const uint8_t* iq, const uint8_t* e, const uint8_t in[256], std::vector<uint8_t>& out) {
    std::vector<uint8_t> blob(sizeof(BCRYPT_RSAKEY_BLOB) + 3 + 256 + 128 * 5 + 256);
    auto* head = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    head->Magic = BCRYPT_RSAFULLPRIVATE_MAGIC;
    head->BitLength = 2048;
    head->cbPublicExp = 3;
    head->cbModulus = 256;
    head->cbPrime1 = head->cbPrime2 = 128;
    uint8_t* at = blob.data() + sizeof(BCRYPT_RSAKEY_BLOB);
    for (auto [src, n] : {std::pair<const uint8_t*, size_t>{e, 3}, {modulus, 256}, {p, 128}, {q, 128}, {dp, 128}, {dq, 128}, {iq, 128}, {d, 256}}) {
        memcpy(at, src, n);
        at += n;
    }
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(Alg().rsa, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, &key, blob.data(), (ULONG)blob.size(), 0) < 0) return false;
    out.assign(256, 0);
    ULONG got = 0;
    const bool ok = BCryptDecrypt(key, (PUCHAR)in, 256, nullptr, nullptr, 0, out.data(), 256, &got, BCRYPT_PAD_PKCS1) >= 0;
    BCryptDestroyKey(key);
    out.resize(ok ? got : 0);
    return ok;
}

// ---- byte sources ----
struct Source {
    virtual ~Source() = default;
    virtual bool Read(uint64_t offset, uint8_t* buf, size_t n) = 0;
    virtual uint64_t Size() const = 0;
};

// A byte range of the PKG file, [begin, end).
struct Range {
    uint64_t begin, end;
};

struct FileSource : Source {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
    uint64_t actual = 0;               // the file's real size (`size` can be its size before a shrinking convert cut it)
    std::vector<Range>* log = nullptr; // PkgLayoutReport / shrinking: every read, in order
    std::wstring path_;
    bool write_ = false;
    bool Open(const std::wstring& path, bool write = false, uint64_t logicalSize = 0) {
        path_ = path;
        write_ = write;
        if (!Reopen()) return false;
        actual = size;
        if (logicalSize > size) size = logicalSize;
        return true;
    }
    bool Reopen() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        CREATEFILE2_EXTENDED_PARAMETERS p{sizeof(p)};
        p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        p.dwFileFlags = FILE_FLAG_RANDOM_ACCESS;
        h = CreateFile2FromAppW(path_.c_str(), GENERIC_READ | (write_ ? GENERIC_WRITE : 0), FILE_SHARE_READ, OPEN_EXISTING, &p); // FromApp: USB drives on the Xbox
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER s{};
        GetFileSizeEx(h, &s);
        size = (uint64_t)s.QuadPart;
        return true;
    }
    // Shrinking convert: drop everything from `end` on. The end of file alone doesn't give the space back on NTFS while
    // the file is open (measured on the PC, 2026-11-12), so the allocation is cut too and the file is opened again.
    bool Cut(uint64_t end) {
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = (LONGLONG)end;
        if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof(info))) return false;
        FILE_ALLOCATION_INFO alloc{};
        alloc.AllocationSize.QuadPart = (LONGLONG)end;
        SetFileInformationByHandle(h, FileAllocationInfo, &alloc, sizeof(alloc));
        const uint64_t logical = size;
        if (!Reopen()) return false;
        actual = end;
        size = logical;
        return true;
    }
    ~FileSource() override { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool Read(uint64_t offset, uint8_t* buf, size_t n) override {
        if (log) log->push_back({offset, offset + n});
        while (n) {
            OVERLAPPED at{};
            at.Offset = DWORD(offset);
            at.OffsetHigh = DWORD(offset >> 32);
            const DWORD chunk = DWORD(std::min<size_t>(n, 1u << 30));
            DWORD got = 0;
            if (!ReadFile(h, buf, chunk, &got, &at) || got == 0) return false;
            buf += got; offset += got; n -= got;
        }
        return true;
    }
    uint64_t Size() const override { return size; }
};

// ---- PFS ----
struct Inode {
    uint16_t mode = 0;
    uint64_t size = 0, blocks = 0;
    int32_t direct[12]{};
    int32_t indirect[5]{};
    bool IsDir() const { return (mode & 0x4000) != 0; }
};
struct Dirent {
    uint32_t inode;
    std::string name;
};

class Pfs {
public:
    bool Open(Source* src, uint64_t base, const uint8_t* ekpfs, std::wstring& err) {
        src_ = src;
        base_ = base;
        uint8_t h[0x400];
        if (!src->Read(base, h, sizeof(h))) { err = L"Couldn't read a PFS header."; return false; }
        mode_ = Le16(h + 0x1C);
        const uint64_t count = Le64(h + 0x30);
        if (count == 0 || count > 10000000) { err = L"Bad PFS header."; return false; }
        if (mode_ & 0x4) { // encrypted: XTS keys from HMAC-SHA256(EKPFS, LE32(1) || seed)
            if (!ekpfs) { err = L"This is an official (retail) PKG: its game files are encrypted with keys only a PS4 has. XStation converts fake PKGs, such as your own backups."; return false; }
            uint8_t msg[20];
            msg[0] = 1; msg[1] = msg[2] = msg[3] = 0;
            memcpy(msg + 4, h + 0x370, 16);
            uint8_t k[32];
            HmacSha256(ekpfs, 32, msg, sizeof(msg), k);
            tweak_ = AesKey(Alg().ecb, k);
            data_ = AesKey(Alg().ecb, k + 16);
            encrypted_ = true;
        }
        const bool is64 = (mode_ & 2) != 0, signedMode = (mode_ & 1) != 0;
        const uint64_t inodeSize = is64 ? 0x310 : signedMode ? 0x2C8 : 0xA8;
        inodes_.resize(count);
        std::vector<uint8_t> block(kBlock);
        int64_t loaded = -1;
        uint64_t pos = kBlock; // the inode table starts at block 1
        for (uint64_t i = 0; i < count; ++i) {
            const int64_t b = int64_t(pos / kBlock);
            if (b != loaded) {
                if (!ReadBlock(b, block.data())) { err = L"Couldn't read the PFS inode table."; return false; }
                loaded = b;
            }
            ParseInode(block.data() + (pos % kBlock), is64, signedMode, inodes_[i]);
            pos += inodeSize;
            if (pos % kBlock > kBlock - inodeSize) pos += kBlock - pos % kBlock;
        }
        return true;
    }

    bool ReadBlock(int64_t block, uint8_t* out) {
        if (!src_->Read(base_ + uint64_t(block) * kBlock, out, kBlock)) return false;
        if (encrypted_) {
            for (int s = 0; s < 16; ++s) {
                const uint64_t sector = uint64_t(block) * 16 + s;
                if (sector >= 16 && !XtsDecrypt(out + s * kSector, sector)) return false; // the header block isn't encrypted
            }
        }
        return true;
    }

    const Inode* Get(uint64_t n) const { return n < inodes_.size() ? &inodes_[n] : nullptr; }
    uint32_t Uroot() const { const Inode* i = Get(2); return i && i->IsDir() ? 2u : 3u; }
    size_t Count() const { return inodes_.size(); }

    // The data blocks of a file, as PfsReader.EnumerateBlocks lists them.
    bool Blocks(const Inode& ino, std::vector<int64_t>& blocks) {
        blocks.clear();
        int64_t last = -1;
        for (int32_t b : ino.direct) {
            if (b == -1) {
                if (last <= 0) break;
                while (blocks.size() < ino.blocks) blocks.push_back(++last);
                return true;
            }
            if (b <= 0) break;
            last = b;
            blocks.push_back(b);
        }
        for (int level = 1; level <= 5; ++level) {
            const int32_t ib = ino.indirect[level - 1];
            if (ib <= 0) break;
            if (!Indirect(ib, level, blocks, last)) return false;
        }
        return true;
    }

    bool Dirents(const Inode& dir, std::vector<Dirent>& out) {
        out.clear();
        std::vector<int64_t> blocks;
        if (!Blocks(dir, blocks)) return false;
        blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](int64_t b) { return b <= 0; }), blocks.end());
        std::vector<uint8_t> data(kBlock), next(kBlock);
        bool havePending = false;
        size_t pendingOff = 0;
        for (size_t bi = 0; bi < blocks.size(); ++bi) {
            size_t off = 0;
            if (havePending) { data.swap(next); off = pendingOff; havePending = false; }
            else if (!ReadBlock(blocks[bi], data.data())) return false;
            while (off + 16 <= kBlock) {
                const uint32_t inode = Le32(&data[off]);
                const int32_t nameLen = (int32_t)Le32(&data[off + 8]), entSize = (int32_t)Le32(&data[off + 12]);
                if (entSize < 16 + nameLen || entSize > 0x400 || nameLen < 0 || nameLen > 0x400) break;
                if (off + 16 + nameLen > kBlock) { // the name continues in the next block (Sony's contiguous layout)
                    const size_t first = kBlock - (off + 16);
                    std::string name(reinterpret_cast<const char*>(&data[off + 16]), first);
                    if (bi + 1 < blocks.size()) {
                        if (!ReadBlock(blocks[bi + 1], next.data())) return false;
                        name.append(reinterpret_cast<const char*>(next.data()), std::min<size_t>(nameLen - first, kBlock));
                    } else {
                        std::fill(next.begin(), next.end(), 0);
                        name.append(nameLen - first, '\0');
                    }
                    name.resize(strnlen(name.c_str(), name.size()));
                    out.push_back({inode, name});
                    havePending = bi + 1 < blocks.size();
                    pendingOff = size_t(entSize - 16) - first;
                    break;
                }
                std::string name(reinterpret_cast<const char*>(&data[off + 16]), size_t(nameLen));
                name.resize(strnlen(name.c_str(), name.size()));
                out.push_back({inode, name});
                off += size_t(entSize);
            }
        }
        return true;
    }

private:
    static void ParseInode(const uint8_t* b, bool is64, bool signedMode, Inode& ino) {
        ino.mode = Le16(b);
        ino.size = Le64(b + 8);
        ino.blocks = is64 ? Le64(b + 96) : Le32(b + 96);
        const int ptr = is64 ? 8 : 4, head = is64 ? 104 : 100, slot = (signedMode ? 32 : 0) + ptr;
        auto read = [&](int off) { return signedMode ? (is64 ? (int32_t)Le64(b + off + 32) : (int32_t)Le32(b + off + 32))
                                                     : (is64 ? (int32_t)Le64(b + off) : (int32_t)Le32(b + off)); };
        for (int i = 0; i < 12; ++i) ino.direct[i] = read(head + i * slot);
        for (int i = 0; i < 5; ++i) ino.indirect[i] = read(head + 12 * slot + i * slot);
    }

    bool Indirect(int64_t block, int depth, std::vector<int64_t>& blocks, int64_t& last) {
        if (block <= 0 || depth <= 0) return true;
        std::vector<uint8_t> data(kBlock);
        if (!ReadBlock(block, data.data())) return false;
        const bool is64 = (mode_ & 2) != 0, signedMode = (mode_ & 1) != 0;
        const size_t ptrSize = is64 ? 8 : 4, entry = signedMode ? 32 + ptrSize : ptrSize, count = kBlock / entry;
        for (size_t i = 0; i < count; ++i) {
            const size_t off = i * entry;
            const int32_t ptr = ptrSize == 8 ? (int32_t)Le64(&data[off + 32]) : (int32_t)Le32(&data[off + (signedMode ? 32 : 0)]);
            if (ptr == -1) {
                if (last <= 0) continue;
                while (i < count - 1) { blocks.push_back(++last); ++i; }
                return true;
            }
            if (ptr <= 0) continue;
            last = ptr;
            if (depth == 1) blocks.push_back(ptr);
            else if (!Indirect(ptr, depth - 1, blocks, last)) return false;
        }
        return true;
    }

    // AES-XTS sector decrypt (IEEE 1619; tweak = sector number, little endian).
    bool XtsDecrypt(uint8_t* sector, uint64_t number) {
        uint8_t tweak[16]{};
        for (int i = 0; i < 8; ++i) tweak[i] = uint8_t(number >> (8 * i));
        ULONG got = 0;
        if (BCryptEncrypt(tweak_.h, tweak, 16, nullptr, nullptr, 0, tweak, 16, &got, 0) < 0) return false;
        alignas(16) uint8_t schedule[kSector];
        for (size_t blk = 0; blk < kSector / 16; ++blk) {
            memcpy(schedule + blk * 16, tweak, 16);
            int feedback = 0;
            for (int k = 0; k < 16; ++k) {
                const uint8_t t = tweak[k];
                tweak[k] = uint8_t((tweak[k] << 1) | feedback);
                feedback = (t & 0x80) >> 7;
            }
            if (feedback) tweak[0] ^= 0x87;
        }
        for (size_t i = 0; i < kSector; ++i) sector[i] ^= schedule[i];
        if (BCryptDecrypt(data_.h, sector, (ULONG)kSector, nullptr, nullptr, 0, sector, (ULONG)kSector, &got, 0) < 0) return false;
        for (size_t i = 0; i < kSector; ++i) sector[i] ^= schedule[i];
        return true;
    }

    Source* src_ = nullptr;
    uint64_t base_ = 0;
    uint16_t mode_ = 0;
    bool encrypted_ = false;
    AesKey tweak_, data_;
    std::vector<Inode> inodes_;
};

// A file inside a PFS, as a byte source (pfs_image.dat inside the outer PFS).
struct PfsFileSource : Source {
    Pfs* pfs = nullptr;
    std::vector<int64_t> blocks;
    uint64_t length = 0;
    int64_t cached = -1;
    std::vector<uint8_t> cache = std::vector<uint8_t>(kBlock);
    bool Read(uint64_t offset, uint8_t* buf, size_t n) override {
        while (n) {
            if (offset >= length) return false;
            const uint64_t index = offset / kBlock;
            if (index >= blocks.size()) return false;
            if ((int64_t)index != cached) {
                if (!pfs->ReadBlock(blocks[index], cache.data())) return false;
                cached = (int64_t)index;
            }
            const size_t in = size_t(offset % kBlock);
            const size_t take = (size_t)std::min<uint64_t>({kBlock - in, n, length - offset});
            memcpy(buf, cache.data() + in, take);
            buf += take; offset += take; n -= take;
        }
        return true;
    }
    uint64_t Size() const override { return length; }
};

// A PFSC (block-compressed) image over another source.
struct PfscSource : Source {
    Source* src = nullptr;
    uint32_t blockSize = 0;
    uint64_t length = 0;
    std::vector<uint64_t> offsets;
    int64_t cached = -1;
    std::vector<uint8_t> cache, packed;
    std::map<int64_t, std::vector<uint8_t>> meta; // the first 64 blocks (header, inodes, directories), as PFSCStream keeps them

    bool Open(Source* source, std::wstring& err) {
        src = source;
        uint8_t h[0x30];
        if (!src->Read(0, h, sizeof(h)) || Le32(h) != 0x43534650) { err = L"Bad PFSC header."; return false; }
        blockSize = Le32(h + 0x0C);
        const uint64_t table = Le64(h + 0x18);
        length = Le64(h + 0x28);
        if (!blockSize || blockSize > (16u << 20)) { err = L"Bad PFSC block size."; return false; }
        const uint64_t count = (length + blockSize - 1) / blockSize;
        std::vector<uint8_t> raw((count + 1) * 8);
        if (!src->Read(table, raw.data(), raw.size())) { err = L"Couldn't read the PFSC block table."; return false; }
        offsets.resize(count + 1);
        for (uint64_t i = 0; i <= count; ++i) offsets[i] = Le64(&raw[i * 8]);
        cache.resize(blockSize);
        return true;
    }

    bool Block(int64_t index, const std::vector<uint8_t>*& out) {
        if (index < 64) {
            auto it = meta.find(index);
            if (it != meta.end()) { out = &it->second; return true; }
        }
        if (index == cached) { out = &cache; return true; }
        const uint64_t count = offsets.size() - 1;
        const uint64_t start = offsets[index], end = offsets[index + 1];
        if (end < start || end - start > (64u << 20)) return false;
        const size_t compressed = size_t(end - start);
        packed.resize(compressed);
        if (compressed && !src->Read(start, packed.data(), compressed)) return false;
        const size_t expected = uint64_t(index) + 1 < count ? blockSize : size_t(length - (count - 1) * uint64_t(blockSize));
        cache.assign(expected, 0);
        if (compressed == blockSize) {
            memcpy(cache.data(), packed.data(), std::min<size_t>(expected, compressed));
        } else {
            const bool zlib = compressed >= 2 && (packed[0] & 0x0F) == 0x08;
            const size_t got = tinfl_decompress_mem_to_mem(cache.data(), expected, packed.data(), compressed,
                                                           zlib ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
            if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) return false;
            cache.resize(got);
        }
        cached = index;
        if (index < 64) { out = &(meta[index] = cache); return true; }
        out = &cache;
        return true;
    }

    bool Read(uint64_t offset, uint8_t* buf, size_t n) override {
        while (n) {
            if (offset >= length) return false;
            const std::vector<uint8_t>* block = nullptr;
            if (!Block(int64_t(offset / blockSize), block)) return false;
            const size_t in = size_t(offset % blockSize);
            if (in >= block->size()) return false;
            const size_t take = (size_t)std::min<uint64_t>({block->size() - in, n, length - offset});
            memcpy(buf, block->data() + in, take);
            buf += take; offset += take; n -= take;
        }
        return true;
    }
    uint64_t Size() const override { return length; }
};

// ---- one PKG ----
struct Entry {
    uint8_t raw[32];
    uint32_t id, nameOffset, flags1, flags2;
    uint64_t offset, size;
    bool Encrypted() const { return (flags1 & 0x80000000u) != 0; }
    int KeyIndex() const { return int((flags2 & 0xF000) >> 12); }
};

const char* KnownName(uint32_t id) {
    switch (id) {
    case 0x400: return "license.dat"; case 0x401: return "license.info"; case 0x402: return "nptitle.dat";
    case 0x403: return "npbind.dat"; case 0x404: return "selfinfo.dat"; case 0x406: return "imageinfo.dat";
    case 0x409: return "psreserved.dat"; case 0x1000: return "param.sfo"; case 0x1001: return "playgo-chunk.dat";
    case 0x1002: return "playgo-chunk.sha"; case 0x1003: return "playgo-manifest.xml"; case 0x1008: return "app/playgo-chunk.dat";
    case 0x408: return "origin-deltainfo.dat"; case 0x1004: return "pronunciation.xml"; case 0x1005: return "pronunciation.sig";
    case 0x1006: return "pic1.png"; case 0x100B: return "shareparam.json"; case 0x100C: return "shareoverlayimage.png";
    case 0x1200: return "icon0.png"; case 0x1220: return "pic0.png"; case 0x1240: return "snd0.at9";
    case 0x1260: return "changeinfo/changeinfo.xml"; case 0x100D: return "save_data.png"; case 0x1280: return "icon0.dds";
    case 0x12A0: return "pic0.dds"; case 0x12C0: return "pic1.dds"; case 0x1400: return "trophy/trophy00.trp";
    default: return nullptr;
    }
}

class Pkg {
public:
    std::string contentId;
    uint32_t contentType = 0;
    std::vector<Entry> entries;
    std::map<uint32_t, std::string> names;
    FileSource file;
    Pfs outer, inner;
    PfsFileSource image;
    PfscSource pfsc;
    bool hasInner = false, hasPfsc = false;

    // The PKG bytes reading [offset, offset + n) of pfs_image.dat touches (outer PFS blocks).
    void ImageToPkg(uint64_t offset, uint64_t n, std::vector<Range>& out) const {
        if (!n) return;
        for (uint64_t b = offset / kBlock; b <= (offset + n - 1) / kBlock && b < image.blocks.size(); ++b) {
            const uint64_t at = pfsOffset_ + uint64_t(image.blocks[b]) * kBlock;
            if (!out.empty() && out.back().end == at) out.back().end = at + kBlock;
            else out.push_back({at, at + kBlock});
        }
    }
    // The PKG bytes reading one inner PFS block touches (through PFSC when the image is compressed).
    void InnerBlockToPkg(int64_t block, std::vector<Range>& out) const {
        const uint64_t at = uint64_t(block) * kBlock;
        if (!hasPfsc) { ImageToPkg(at, kBlock, out); return; }
        const uint64_t count = pfsc.offsets.size() - 1;
        for (uint64_t i = at / pfsc.blockSize; i <= (at + kBlock - 1) / pfsc.blockSize && i < count; ++i)
            ImageToPkg(pfsc.offsets[i], pfsc.offsets[i + 1] - pfsc.offsets[i], out);
    }
    uint64_t PfsOffset() const { return pfsOffset_; }

    Source* raw = &file; // the PKG's bytes: the file, or a PKG arriving over Wi-Fi (PkgStream)

    bool write = false;      // shrinking convert: open for writing (it cuts the file short)
    uint64_t logicalSize = 0; // shrinking convert resumed: the PKG's size before it was cut

    bool OpenHeader(const std::wstring& path, std::wstring& err) {
        if (!file.Open(path, write, logicalSize)) { err = L"Couldn't open the PKG (error " + std::to_wstring(GetLastError()) + L")."; return false; }
        raw = &file;
        return ParseHeader(err);
    }
    // Header, tables, keys and the file system from another source.
    bool OpenSource(Source* source, std::wstring& err) {
        raw = source;
        return ParseHeader(err) && OpenTables(err) && OpenImage(err);
    }
    bool ParseHeader(std::wstring& err) {
        uint8_t h[0x500];
        if (raw->Size() < sizeof(h) || !raw->Read(0, h, sizeof(h)) || Be32(h) != 0x7F434E54) { err = L"Not a PS4 PKG file."; return false; }
        entryCount_ = Be32(h + 0x10);
        entryTable_ = Be32(h + 0x18);
        contentType = Be32(h + 0x74);
        contentId.assign(reinterpret_cast<const char*>(h + 0x40), strnlen(reinterpret_cast<const char*>(h + 0x40), 0x30));
        pfsOffset_ = Be64(h + 0x410);
        pfsSize_ = Be64(h + 0x418);
        return true;
    }

    bool Open(const std::wstring& path, std::wstring& err) { return OpenEntries(path, err) && OpenImage(err); }

    // Header, entry table, names and keys (enough for the Sc0 files such as param.sfo).
    bool OpenEntries(const std::wstring& path, std::wstring& err) { return OpenHeader(path, err) && OpenTables(err); }
    bool OpenTables(std::wstring& err) {
        if (uint64_t(entryTable_) + uint64_t(entryCount_) * 32 > raw->Size() || entryCount_ > 100000) { err = L"The PKG's entry table is out of bounds."; return false; }
        std::vector<uint8_t> table(size_t(entryCount_) * 32);
        if (!raw->Read(entryTable_, table.data(), table.size())) { err = L"Couldn't read the PKG's entry table."; return false; }
        for (uint32_t i = 0; i < entryCount_; ++i) {
            Entry e{};
            memcpy(e.raw, &table[i * 32], 32);
            e.id = Be32(e.raw); e.nameOffset = Be32(e.raw + 4); e.flags1 = Be32(e.raw + 8); e.flags2 = Be32(e.raw + 12);
            e.offset = Be32(e.raw + 16); e.size = Be32(e.raw + 20);
            entries.push_back(e);
        }
        if (const Entry* n = Find(0x200)) ReadNames(*n);
        return Keys(err);
    }

    const Entry* Find(uint32_t id) const {
        for (const auto& e : entries) if (e.id == id) return &e;
        return nullptr;
    }

    std::string Name(const Entry& e) const {
        auto it = names.find(e.nameOffset);
        if (it != names.end()) return it->second;
        const char* known = KnownName(e.id);
        return known ? known : "";
    }

    // Entry data, decrypted when the entry is (AES-128-CBC with SHA256(entry record || dk[key index])).
    bool CopyEntry(const Entry& e, const std::function<bool(const uint8_t*, size_t)>& write, const std::atomic<bool>* cancel) {
        const uint64_t stored = e.Encrypted() ? (e.size + 15) & ~15ull : e.size;
        if (e.offset + stored > raw->Size()) return false;
        uint8_t key[16], iv[16];
        if (e.Encrypted()) EntryKey(e, dk_[e.KeyIndex() & 7], iv, key);
        std::vector<uint8_t> buf(1u << 20);
        uint64_t at = e.offset, left = stored, logical = e.size;
        while (left) {
            if (cancel && *cancel) return false;
            const size_t n = (size_t)std::min<uint64_t>(buf.size(), left);
            if (!raw->Read(at, buf.data(), n)) return false;
            if (e.Encrypted() && !AesCbcDecrypt(key, iv, buf.data(), n)) return false;
            const size_t keep = (size_t)std::min<uint64_t>(logical, n);
            if (keep && !write(buf.data(), keep)) return false;
            at += n; left -= n; logical -= keep;
        }
        return true;
    }

private:
    void ReadNames(const Entry& e) {
        if (e.offset + e.size > raw->Size() || e.size > (64u << 20)) return;
        std::vector<char> data(size_t(e.size) + 1, 0);
        if (!raw->Read(e.offset, reinterpret_cast<uint8_t*>(data.data()), size_t(e.size))) return;
        size_t pos = 0;
        uint32_t offset = 0;
        while (pos < e.size) {
            const size_t len = strnlen(&data[pos], std::min<size_t>(2048, size_t(e.size) - pos));
            if (len && !names.count(offset)) names[offset] = std::string(&data[pos], len);
            offset += uint32_t(len + 1);
            pos += len + 1;
            if (!len && offset > 1) break;
        }
    }

    static void EntryKey(const Entry& e, const uint8_t dk[32], uint8_t iv[16], uint8_t key[16]) {
        uint8_t material[64], h[32];
        memcpy(material, e.raw, 32);
        memcpy(material + 32, dk, 32);
        Sha256(material, 64, h);
        memcpy(iv, h, 16);
        memcpy(key, h + 16, 16);
    }

    // dk_i = SHA256(SHA256(BE32 i) || SHA256(content id padded to 48) || passcode); the passcode is 32 '0's (fake PKGs).
    // When none matches ENTRY_KEYS' digests, dk3 is RSA-recovered from ENTRY_KEYS[3] (official PKGs).
    bool Keys(std::wstring& err) {
        uint8_t cid[48]{}, cidHash[32];
        memcpy(cid, contentId.data(), std::min<size_t>(contentId.size(), 36));
        Sha256(cid, 48, cidHash);
        for (uint32_t i = 0; i < 7; ++i) {
            uint8_t be[4] = {uint8_t(i >> 24), uint8_t(i >> 16), uint8_t(i >> 8), uint8_t(i)};
            uint8_t data[96];
            Sha256(be, 4, data);
            memcpy(data + 32, cidHash, 32);
            memset(data + 64, '0', 32);
            Sha256(data, 96, dk_[i]);
        }
        const Entry* keys = Find(0x10);
        if (!keys || keys->size < 32 + 7 * 32) return true;
        std::vector<uint8_t> data(size_t(std::min<uint64_t>(keys->size, 32 + 7 * 32 + 7 * 256)));
        if (!raw->Read(keys->offset, data.data(), data.size())) { err = L"Couldn't read the PKG's keys."; return false; }
        auto matches = [&](int i, const uint8_t* dk) {
            uint8_t h[32];
            Sha256(dk, 32, h);
            for (int j = 0; j < 32; ++j) h[j] ^= dk[j];
            return memcmp(h, &data[32 + i * 32], 32) == 0;
        };
        for (int i = 0; i < 7; ++i) if (matches(i, dk_[i])) return true;
#ifdef XSTATION_PUBLIC
        err = L"This PKG isn't a fake PKG (an official Sony package, or one with its own passcode). XStation converts fake PKGs only.";
        return false;
#else
        std::vector<uint8_t> recovered;
        if (data.size() >= 32 + 7 * 32 + 4 * 256 &&
            RsaDecrypt(kDerivedKey3Modulus, kDerivedKey3D, kDerivedKey3P, kDerivedKey3Q, kDerivedKey3DP, kDerivedKey3DQ, kDerivedKey3InverseQ,
                       kDerivedKey3Exponent, &data[32 + 7 * 32 + 3 * 256], recovered) && recovered.size() == 32 && matches(3, recovered.data())) {
            memcpy(dk_[3], recovered.data(), 32);
            return true;
        }
        err = L"This PKG uses a passcode or keys XStation doesn't have (a retail or custom-key package).";
        return false;
#endif
    }

    // EKPFS from IMAGE_KEY (AES-CBC with dk3, then RSA with the fake key set); then the outer PFS, pfs_image.dat in it, the
    // PFSC image in that, and the inner PFS with the game files.
    bool OpenImage(std::wstring& err) {
        if (!pfsOffset_ || pfsOffset_ + pfsSize_ > raw->Size()) return true; // no game files (some add-ons)
        uint8_t ekpfs[32];
        bool haveKey = false;
        if (const Entry* ik = Find(0x20)) {
            std::vector<uint8_t> data(size_t(ik->size));
            if (ik->size >= 256 && raw->Read(ik->offset, data.data(), data.size())) {
                uint8_t iv[16], key[16];
                EntryKey(*ik, dk_[3], iv, key);
                data.resize(data.size() & ~size_t(15));
                std::vector<uint8_t> out;
                if (AesCbcDecrypt(key, iv, data.data(), data.size()) &&
                    RsaDecrypt(kFakeKeysetModulus, kFakeKeysetD, kFakeKeysetP, kFakeKeysetQ, kFakeKeysetDP, kFakeKeysetDQ, kFakeKeysetInverseQ,
                               kFakeKeysetExponent, data.data(), out) && out.size() == 32) {
                    memcpy(ekpfs, out.data(), 32);
                    haveKey = true;
                }
            }
        }
        if (!outer.Open(raw, pfsOffset_, haveKey ? ekpfs : nullptr, err)) return false;
        const Inode* imageFile = FindImage();
        if (!imageFile) { err = L"pfs_image.dat is missing from the PKG."; return false; }
        image.pfs = &outer;
        image.length = imageFile->size;
        if (!outer.Blocks(*imageFile, image.blocks)) { err = L"Couldn't read pfs_image.dat's block list."; return false; }
        image.blocks.erase(std::remove_if(image.blocks.begin(), image.blocks.end(), [](int64_t b) { return b <= 0; }), image.blocks.end());
        uint8_t magic[4];
        Source* innerSource = &image;
        if (image.Read(0, magic, 4) && !memcmp(magic, "PFSC", 4)) {
            if (!pfsc.Open(&image, err)) return false;
            innerSource = &pfsc;
            hasPfsc = true;
        }
        if (!inner.Open(innerSource, 0, haveKey ? ekpfs : nullptr, err)) return false;
        hasInner = true;
        return true;
    }

    // pfs_image.dat in the outer PFS: through the flat path table (inode 1), else the root's entries.
    const Inode* FindImage() {
        if (const Inode* fpt = outer.Get(1)) {
            uint32_t want = 0;
            for (char c : std::string("/pfs_image.dat")) want = uint32_t(toupper((unsigned char)c)) + 31 * want;
            std::vector<int64_t> blocks;
            if (outer.Blocks(*fpt, blocks)) {
                std::vector<uint8_t> data;
                std::vector<uint8_t> block(kBlock);
                for (int64_t b : blocks) {
                    if (b <= 0 || data.size() >= fpt->size || !outer.ReadBlock(b, block.data())) break;
                    data.insert(data.end(), block.begin(), block.begin() + (size_t)std::min<uint64_t>(kBlock, fpt->size - data.size()));
                }
                for (size_t i = 0; i + 8 <= data.size(); i += 8) {
                    const uint32_t hash = Le32(&data[i]), ino = Le32(&data[i + 4]);
                    if (hash == want && (ino & 0x0FFFFFFF) < outer.Count()) return outer.Get(ino & 0x0FFFFFFF);
                }
            }
        }
        if (const Inode* root = outer.Get(outer.Uroot())) {
            std::vector<Dirent> list;
            if (outer.Dirents(*root, list))
                for (const auto& d : list)
                    if (_stricmp(d.name.c_str(), "pfs_image.dat") == 0 && d.inode < outer.Count()) return outer.Get(d.inode);
        }
        return nullptr;
    }

    uint32_t entryCount_ = 0, entryTable_ = 0;
    uint64_t pfsOffset_ = 0, pfsSize_ = 0;
    uint8_t dk_[7][32]{};
};

// ---- the merged plan ----
struct Item {
    size_t pkg;
    bool sc0;
    size_t index; // entry index (Sc0) or inode number (Image0)
    uint64_t size;
    std::string path; // relative, '/' separated
};

bool SafePath(const std::string& path) {
    if (path.empty() || path[0] == '/' || path.find(':') != std::string::npos || path.find('\\') != std::string::npos) return false;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = std::min(path.find('/', start), path.size());
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

void WalkTree(Pfs& pfs, uint32_t dirInode, const std::string& prefix, size_t pkg, std::vector<Item>& out, std::set<uint32_t>& visited) {
    const Inode* dir = pfs.Get(dirInode);
    std::vector<Dirent> list;
    if (!dir || !pfs.Dirents(*dir, list)) return;
    for (const auto& d : list) {
        if (d.name == "." || d.name == ".." || d.name == "flat_path_table" || d.inode >= pfs.Count()) continue;
        const Inode* ino = pfs.Get(d.inode);
        if (!ino) continue;
        const std::string path = prefix.empty() ? d.name : prefix + "/" + d.name;
        if (ino->IsDir()) {
            if (!visited.insert(d.inode).second) continue;
            WalkTree(pfs, d.inode, path, pkg, out, visited);
            visited.erase(d.inode);
        } else {
            out.push_back({pkg, false, d.inode, ino->size, path});
        }
    }
}

bool MakeParents(const std::wstring& path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos; at = path.find(L'\\', at + 1)) {
        const std::wstring dir = path.substr(0, at);
        if (!CreateDirectoryFromAppW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (!GetFileAttributesExFromAppW(dir.c_str(), GetFileExInfoStandard, &a) || !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
        }
    }
    return true;
}

// 2026-11-14: every file under <dir> with its size, by lower-case full path (one listing per folder, not a lookup per file).
void ListFiles(const std::wstring& dir, std::unordered_map<std::wstring, uint64_t>& out) {
    WIN32_FIND_DATAW fd{};
    HANDLE f = FindFirstFileExFromAppW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        const std::wstring path = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ListFiles(path, out); continue; }
        std::wstring key = path;
        for (wchar_t& c : key) c = (wchar_t)towlower(c);
        out[key] = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

// 2026-11-14: MakeParents, but each folder only once per conversion (it cost a call per level per file).
struct Folders {
    std::set<std::wstring> made;
    bool For(const std::wstring& path) {
        const size_t last = path.find_last_of(L'\\');
        if (last == std::wstring::npos || made.count(path.substr(0, last))) return true;
        for (size_t at = path.find(L'\\', 3); at != std::wstring::npos && at <= last; at = path.find(L'\\', at + 1)) {
            const std::wstring dir = path.substr(0, at);
            if (made.count(dir)) continue;
            if (!CreateDirectoryFromAppW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
                WIN32_FILE_ATTRIBUTE_DATA a{};
                if (!GetFileAttributesExFromAppW(dir.c_str(), GetFileExInfoStandard, &a) || !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
            }
            made.insert(dir);
        }
        return true;
    }
};

// Writes through a 4 MiB buffer into <path>.part, renamed when complete.
struct Writer {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::vector<uint8_t> buf;
    size_t used = 0;
    bool Open(const std::wstring& part) {
        h = CreateFile2FromAppW(part.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        buf.resize(4u << 20);
        return h != INVALID_HANDLE_VALUE;
    }
    bool Flush() {
        DWORD wrote = 0;
        if (used && (!WriteFile(h, buf.data(), DWORD(used), &wrote, nullptr) || wrote != used)) return false;
        used = 0;
        return true;
    }
    bool Write(const uint8_t* p, size_t n) {
        while (n) {
            const size_t take = std::min(n, buf.size() - used);
            memcpy(buf.data() + used, p, take);
            used += take; p += take; n -= take;
            if (used == buf.size() && !Flush()) return false;
        }
        return true;
    }
    ~Writer() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

// The PKG bytes one planned file reads (Sc0 entry, or its inner PFS blocks through PFSC and the outer PFS).
bool RangesOf(Pkg& pkg, const Item& it, std::vector<Range>& out) {
    if (it.sc0) {
        const Entry& en = pkg.entries[it.index];
        const uint64_t stored = en.Encrypted() ? (en.size + 15) & ~15ull : en.size;
        if (stored) out.push_back({en.offset, en.offset + stored});
        return true;
    }
    const Inode* ino = pkg.inner.Get(it.index);
    std::vector<int64_t> blocks;
    if (!ino || !pkg.inner.Blocks(*ino, blocks)) return false;
    uint64_t left = ino->size;
    for (size_t b = 0; b < blocks.size() && left && blocks[b] > 0; ++b) {
        pkg.InnerBlockToPkg(blocks[b], out);
        left -= std::min<uint64_t>(left, kBlock);
    }
    return true;
}

// Shrinking convert: a written file's data and directory entry on the drive before the PKG bytes it came from go.
bool FlushPath(const std::wstring& path) {
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

bool ReadSmallFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD got = 0;
    out.clear();
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got && out.size() < (1u << 20)) out.append(buf, got);
    CloseHandle(h);
    return true;
}

bool WriteSmallFile(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, data.data(), DWORD(data.size()), &wrote, nullptr) && wrote == data.size() && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

std::string LowerUtf8(const std::wstring& w) {
    std::wstring l = w;
    for (wchar_t& c : l) c = (wchar_t)towlower(c);
    const int n = WideCharToMultiByte(CP_UTF8, 0, l.c_str(), (int)l.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, l.c_str(), (int)l.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// param.sfo: "\0PSF", key table offset at 8, value table at 12, count at 16, 16-byte entries from 20.
bool ParseSfo(const std::vector<uint8_t>& sfo, PkgParams& out) {
    if (sfo.size() < 20 || memcmp(sfo.data(), "\0PSF", 4)) return false;
    const uint32_t keys = Le32(&sfo[8]), values = Le32(&sfo[12]), count = Le32(&sfo[16]);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= sfo.size(); ++i) {
        const uint8_t* ent = &sfo[20 + i * 16];
        const uint32_t key = keys + Le16(ent), format = Le16(ent + 2), len = Le32(ent + 4), value = values + Le32(ent + 12);
        if (key >= sfo.size() || format != 0x0204 || value + len > sfo.size()) continue;
        const std::string name(reinterpret_cast<const char*>(&sfo[key]), strnlen(reinterpret_cast<const char*>(&sfo[key]), sfo.size() - key));
        const std::string text(reinterpret_cast<const char*>(&sfo[value]), strnlen(reinterpret_cast<const char*>(&sfo[value]), len));
        if (name == "CATEGORY") out.category = text;
        else if (name == "APP_VER") out.appVer = text;
        else if (name == "VERSION") out.version = text;
        else if (name == "TITLE") out.title = text;
        else if (name == "TITLE_ID") out.titleId = text;
    }
    return true;
}

} // namespace

std::wstring NotEnoughStorageText(uint64_t need, uint64_t freeBytes, const std::wstring& where, const std::wstring& lead) {
    // Tenths of a GB: what is needed rounded up, what is free rounded down, so the numbers never say it would fit.
    const uint64_t tenth = 107374182; // 0.1 GiB
    const uint64_t needT = (need + tenth - 1) / tenth, freeT = freeBytes / tenth;
    const uint64_t moreT = needT > freeT ? needT - freeT : 1;
    auto gb = [](uint64_t t) { wchar_t text[32]; swprintf_s(text, L"%llu.%llu GB", t / 10, t % 10); return std::wstring(text); };
    return lead + L" This game needs " + gb(needT) + L" of free space on " + where + L", and it has " + gb(freeT) +
           L" free.\nFree up at least " + gb(moreT) + L" (delete games or files you don't need) and try again.";
}

bool PkgReadInfo(const std::wstring& path, PkgInfo& info, std::wstring& error) {
    Pkg pkg;
    if (!pkg.OpenHeader(path, error)) return false;
    info.contentId = pkg.contentId;
    info.titleId = pkg.contentId.size() >= 16 ? pkg.contentId.substr(7, 9) : "";
    info.contentType = pkg.contentType;
    info.pkgSize = pkg.raw->Size();
    return true;
}

bool PkgReadParams(const std::wstring& path, PkgParams& out, std::wstring& error) {
    if (!Alg().ok) { error = L"Windows' crypto functions couldn't be opened."; return false; }
    Pkg pkg;
    if (!pkg.OpenEntries(path, error)) return false;
    const Entry* e = pkg.Find(0x1000);
    if (!e || e->size > (1u << 20)) { error = L"The PKG has no param.sfo."; return false; }
    std::vector<uint8_t> sfo;
    if (!pkg.CopyEntry(*e, [&](const uint8_t* p, size_t n) { sfo.insert(sfo.end(), p, p + n); return true; }, nullptr)) {
        error = L"Couldn't read the PKG's param.sfo.";
        return false;
    }
    if (!ParseSfo(sfo, out)) { error = L"The PKG's param.sfo is damaged."; return false; }
    // The PKG's own icon0.png (Sc0 entry 0x1200), for the dashboard's list.
    if (const Entry* icon = pkg.Find(0x1200); icon && icon->size < (4u << 20)) {
        if (!pkg.CopyEntry(*icon, [&](const uint8_t* p, size_t n) { out.icon.insert(out.icon.end(), p, p + n); return true; }, nullptr)) out.icon.clear();
    }
    return true;
}

bool PkgExtractGame(const std::vector<std::wstring>& paths, const std::wstring& outDir, PkgProgress* progress,
                    const std::atomic<bool>* cancel, bool resume, std::wstring& error, uint64_t freeBytes, PkgShrink* shrink,
                    bool rewriteUpdates) {
    if (!Alg().ok) { error = L"Windows' crypto functions couldn't be opened."; return false; }
    // A shrinking convert that already cut these PKGs left <game folder>\.xstation-shrink: "<size before>\t<file name>" lines.
    const std::wstring statePath = outDir.substr(0, outDir.find_last_of(L'\\')) + L"\\.xstation-shrink";
    std::map<std::string, uint64_t> before;
    {
        std::string text;
        if (ReadSmallFile(statePath, text)) {
            size_t at = 0;
            while (at < text.size()) {
                size_t end = text.find('\n', at);
                if (end == std::string::npos) end = text.size();
                const std::string line = text.substr(at, end - at);
                const size_t tab = line.find('\t');
                if (tab != std::string::npos) before[line.substr(tab + 1)] = _strtoui64(line.c_str(), nullptr, 10);
                at = end + 1;
            }
        }
    }
    const bool forced = !before.empty();
    const bool mayShrink = forced || (shrink && shrink->allowed);
    std::vector<std::wstring> names;
    std::vector<std::vector<Range>> logs(paths.size());
    std::vector<std::unique_ptr<Pkg>> pkgs;
    for (size_t k = 0; k < paths.size(); ++k) {
        const std::wstring& p = paths[k];
        names.push_back(p.substr(p.find_last_of(L'\\') + 1));
        auto pkg = std::make_unique<Pkg>();
        pkg->write = mayShrink;
        if (auto b = before.find(LowerUtf8(names.back())); b != before.end()) pkg->logicalSize = b->second;
        else if (forced) { error = L"This conversion was using up other PKGs (" + Wide(before.begin()->first) + L"). Convert that PKG again to finish it."; return false; }
        pkg->file.log = &logs[k];
        std::wstring err;
        if (!pkg->Open(p, err)) { error = names.back() + L": " + err; return false; }
        pkgs.push_back(std::move(pkg));
    }
    // Image0 of every PKG in order, then Sc0 of every PKG in order into sce_sys; later wins (the PC import tool's merge).
    std::vector<Item> items;
    for (size_t k = 0; k < pkgs.size(); ++k) {
        if (!pkgs[k]->hasInner) continue;
        std::set<uint32_t> visited;
        WalkTree(pkgs[k]->inner, pkgs[k]->inner.Uroot(), "", k, items, visited);
    }
    for (size_t k = 0; k < pkgs.size(); ++k) {
        const auto& entries = pkgs[k]->entries;
        for (size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            if (e.id == 0x1 || e.id == 0x10 || e.id == 0x20 || e.id == 0x80 || e.id == 0x100 || e.id == 0x200) continue;
            std::string name = pkgs[k]->Name(e);
            if (name.rfind("Sc0/", 0) == 0) name = name.substr(4);
            if (name.empty()) { char synthetic[32]; sprintf_s(synthetic, "entry_%04X.bin", e.id); name = synthetic; }
            items.push_back({k, true, i, e.size, "sce_sys/" + name});
        }
    }
    std::map<std::string, Item> plan; // by lower-case path (Windows names), last one wins
    std::vector<std::string> order;
    for (auto& it : items) {
        if (!SafePath(it.path)) { error = L"The PKG has an unsafe file name: " + Wide(it.path); return false; }
        std::string key = it.path;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (!plan.count(key)) order.push_back(key);
        plan[key] = it;
    }
    auto destOf = [&](const Item& it) {
        std::wstring dest = outDir + L"\\" + Wide(it.path);
        std::replace(dest.begin(), dest.end(), L'/', L'\\');
        return dest;
    };
    // Files already there from an earlier, stopped run: listed once (2026-11-14: was a lookup per file, twice).
    std::unordered_map<std::wstring, uint64_t> existing;
    if (resume) ListFiles(outDir, existing);
    auto present = [&](const Item& it) {
        if (!resume || (rewriteUpdates && !forced && it.pkg > 0)) return false;
        std::wstring key = destOf(it);
        for (wchar_t& c : key) c = (wchar_t)towlower(c);
        const auto e = existing.find(key);
        return e != existing.end() && e->second == it.size;
    };
    uint64_t total = 0, needed = 0;
    for (const auto& [key, it] : plan) {
        total += it.size;
        if (!present(it)) needed += it.size;
    }
    const uint64_t normalNeed = needed + (512ull << 20);
    const bool normalFits = !freeBytes || normalNeed <= freeBytes;

    // Using the PKGs up: each PKG's files from its end backwards; after a file is written, the PKG is cut to the furthest
    // byte a file still to come (or the PKG's own tables) needs, in steps of at least kCutStep.
    constexpr uint64_t kCutStep = 256ull << 20;
    struct Job { std::string key; uint64_t first = UINT64_MAX, last = 0; };
    std::vector<std::vector<Job>> jobs(pkgs.size());
    std::vector<std::vector<uint64_t>> after(pkgs.size()); // after[k][i]: the furthest end jobs i+1.. of PKG k need
    std::vector<uint64_t> floorEnd(pkgs.size(), 0);         // the PKG's header, tables and directories (read while planning)
    uint64_t shrinkNeed = 0;
    if (forced || !normalFits) {
        for (const auto& [key, it] : plan) {
            if (present(it)) continue;
            std::vector<Range> ranges;
            if (!RangesOf(*pkgs[it.pkg], it, ranges)) { error = L"Couldn't read the PKG's file table."; return false; }
            Job j{key};
            for (const auto& r : ranges) { j.first = std::min(j.first, r.begin); j.last = std::max(j.last, r.end); }
            if (ranges.empty()) j.first = j.last = 0;
            jobs[it.pkg].push_back(j);
        }
        for (size_t k = 0; k < pkgs.size(); ++k) {
            for (const auto& r : logs[k]) floorEnd[k] = std::max(floorEnd[k], r.end);
            floorEnd[k] = std::min(floorEnd[k], pkgs[k]->file.size);
            auto& list = jobs[k];
            // Furthest end first: after each file the PKG can be cut to the next file's end (a file spread over the PKG
            // would otherwise hold the cut back until it comes up).
            std::sort(list.begin(), list.end(), [](const Job& a, const Job& b) { return a.last != b.last ? a.last > b.last : a.first > b.first; });
            after[k].assign(list.size(), 0);
            uint64_t furthest = 0; // ("far" is a windows.h macro)
            for (size_t i = list.size(); i-- > 0;) { after[k][i] = furthest; furthest = std::max(furthest, list[i].last); }
        }
        // Peak extra space: written bytes minus the PKG bytes already cut, the way the loop below goes.
        int64_t delta = 0, peak = 0;
        for (size_t k = 0; k < pkgs.size(); ++k) {
            uint64_t cur = pkgs[k]->file.actual;
            auto cutTo = [&](uint64_t target) {
                if (cur > target) { delta -= int64_t(cur - target); cur = target; }
            };
            const auto& list = jobs[k];
            cutTo(std::max(floorEnd[k], list.empty() ? 0 : std::max(list[0].last, after[k][0])));
            for (size_t i = 0; i < list.size(); ++i) {
                delta += int64_t(plan[list[i].key].size);
                peak = std::max(peak, delta);
                const uint64_t target = std::max(floorEnd[k], after[k][i]);
                if (cur > target && (cur - target >= kCutStep || i + 1 == list.size())) cutTo(target);
            }
        }
        shrinkNeed = uint64_t(peak) + (512ull << 20);
        if (shrink) { shrink->need = shrinkNeed; shrink->normalNeed = normalNeed; }
    }
    for (auto& log : logs) { log.clear(); log.shrink_to_fit(); }
    for (auto& pkg : pkgs) pkg->file.log = nullptr;

    const bool shrinking = forced || (!normalFits && mayShrink);
    // 2026-11-18: the drive by its letter, as the dashboard shows it ("D: 39.1 GB free of 136 GB").
    const std::wstring where = outDir.size() > 1 && outDir[1] == L':' ? L"drive " + outDir.substr(0, 2) : L"the drive";
    if (!normalFits && !shrinking) {
        error = NotEnoughStorageText(normalNeed, freeBytes, where);
        return false;
    }
    if (shrinking && freeBytes && shrinkNeed > freeBytes) {
        error = NotEnoughStorageText(shrinkNeed, freeBytes, where, L"Not enough storage, even using the PKG up.");
        return false;
    }
    if (progress) { progress->total = total; progress->filesTotal = (uint32_t)plan.size(); }

    std::vector<uint8_t> block(kBlock);
    Folders folders;
    // One planned file's bytes from the PKG, in order, to `sink`.
    auto readItem = [&](const Item& it, const std::function<bool(const uint8_t*, size_t)>& sink) -> bool {
        Pkg& pkg = *pkgs[it.pkg];
        if (it.sc0) return pkg.CopyEntry(pkg.entries[it.index], [&](const uint8_t* p, size_t n) {
            if (progress) progress->done += n;
            return sink(p, n);
        }, cancel);
        const Inode* ino = pkg.inner.Get(it.index);
        std::vector<int64_t> blocks;
        if (!ino || !pkg.inner.Blocks(*ino, blocks)) return false;
        uint64_t left = ino->size;
        for (size_t b = 0; b < blocks.size() && left; ++b) {
            if (blocks[b] <= 0) break;
            if ((cancel && *cancel) || !pkg.inner.ReadBlock(blocks[b], block.data())) return false;
            const size_t n = (size_t)std::min<uint64_t>(kBlock, left);
            if (!sink(block.data(), n)) return false;
            left -= n;
            if (progress) progress->done += n;
        }
        return !left;
    };
    // One planned file into <dest>.part, then renamed.
    auto extract = [&](const Item& it) -> bool {
        const std::wstring dest = destOf(it);
        if (!folders.For(dest)) { error = L"Couldn't make the folder for " + Wide(it.path) + L"."; return false; }
        const std::wstring part = dest + L".part";
        bool ok;
        {
            Writer w;
            if (!w.Open(part)) { error = L"Couldn't write " + Wide(it.path) + L" (error " + std::to_wstring(GetLastError()) + L")."; return false; }
            ok = readItem(it, [&](const uint8_t* p, size_t n) { return w.Write(p, n); });
            ok = ok && w.Flush();
        }
        if (ok) DeleteFileFromAppW(dest.c_str()); // MoveFileFromAppW doesn't replace
        if (!ok || !MoveFileFromAppW(part.c_str(), dest.c_str())) {
            DeleteFileFromAppW(part.c_str());
            error = (cancel && *cancel) ? L"Stopped." : L"Couldn't extract " + Wide(it.path) + L". The drive may be full or unplugged, or the PKG is damaged.";
            return false;
        }
        if (progress) ++progress->files;
        return true;
    };

    if (!shrinking) {
        // 2026-11-14: games with many small files (God of War: 57,418 files, 55,789 under 1 MB, 12,360 in one folder)
        // crawled on a USB drive, about 5 files a second: on the Xbox every file call goes through the broker, and each
        // file cost about 11 (a CreateDirectory per folder level, a lookup twice, .part, delete, rename). Now folders
        // are made once, files on hand are listed once, and files up to 8 MiB are read here and written straight to
        // their name by 8 writer threads (a stopped one is short, so a resumed run writes it again). Bigger files still
        // go through .part.
        constexpr size_t kSmall = 8u << 20, kInFlight = 256u << 20, kWriters = 8;
        struct Small { std::wstring dest; std::string path; std::vector<uint8_t> data; };
        std::mutex lock;
        std::condition_variable work, room;
        std::deque<Small> queue;
        size_t inFlight = 0;
        bool closing = false;
        std::wstring writeError;
        auto writer = [&] {
            for (;;) {
                Small f;
                {
                    std::unique_lock<std::mutex> l(lock);
                    work.wait(l, [&] { return closing || !queue.empty(); });
                    if (queue.empty()) return;
                    f = std::move(queue.front());
                    queue.pop_front();
                }
                bool ok = false;
                DWORD why = 0;
                HANDLE h = CreateFile2FromAppW(f.dest.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD wrote = 0;
                    ok = f.data.empty() || (WriteFile(h, f.data.data(), DWORD(f.data.size()), &wrote, nullptr) && wrote == f.data.size());
                    if (!ok) why = GetLastError();
                    CloseHandle(h);
                    if (!ok) DeleteFileFromAppW(f.dest.c_str());
                } else why = GetLastError();
                if (ok && progress) ++progress->files;
                {
                    std::lock_guard<std::mutex> l(lock);
                    inFlight -= f.data.size();
                    if (!ok && writeError.empty())
                        writeError = L"Couldn't write " + Wide(f.path) + L" (error " + std::to_wstring(why) + L"). The drive may be full or unplugged.";
                }
                room.notify_all();
            }
        };
        std::vector<std::thread> pool;
        for (size_t i = 0; i < kWriters; ++i) pool.emplace_back(writer);
        auto finish = [&](bool ok) {
            {
                std::lock_guard<std::mutex> l(lock);
                closing = true;
            }
            work.notify_all();
            for (auto& t : pool) t.join();
            if (ok && !writeError.empty()) { error = writeError; return false; }
            return ok;
        };
        for (const auto& key : order) {
            const Item& it = plan[key];
            if (cancel && *cancel) { error = L"Stopped."; return finish(false); }
            {
                std::lock_guard<std::mutex> l(lock);
                if (!writeError.empty()) break;
            }
            if (present(it)) {
                if (progress) { progress->done += it.size; ++progress->files; }
                continue;
            }
            if (it.size > kSmall) {
                if (!extract(it)) return finish(false);
                continue;
            }
            Small f{destOf(it), it.path, {}};
            if (!folders.For(f.dest)) { error = L"Couldn't make the folder for " + Wide(it.path) + L"."; return finish(false); }
            f.data.reserve((size_t)it.size);
            if (!readItem(it, [&](const uint8_t* p, size_t n) { f.data.insert(f.data.end(), p, p + n); return true; })) {
                error = (cancel && *cancel) ? L"Stopped." : L"Couldn't extract " + Wide(it.path) + L". The PKG may be damaged.";
                return finish(false);
            }
            {
                std::unique_lock<std::mutex> l(lock);
                room.wait(l, [&] { return inFlight == 0 || inFlight + f.data.size() <= kInFlight || !writeError.empty(); });
                inFlight += f.data.size();
                queue.push_back(std::move(f));
            }
            work.notify_one();
        }
        return finish(true);
    }

    // Shrinking. Files already there (resumed) count as done.
    for (const auto& [key, it] : plan)
        if (present(it) && progress) { progress->done += it.size; ++progress->files; }
    bool stateWritten = forced;
    std::vector<std::wstring> unflushed; // written since the last cut
    auto cut = [&](size_t k, uint64_t target) -> bool {
        FileSource& f = pkgs[k]->file;
        if (f.actual <= target) return true;
        if (!stateWritten) {
            std::string text;
            for (size_t j = 0; j < pkgs.size(); ++j) text += std::to_string(pkgs[j]->file.size) + "\t" + LowerUtf8(names[j]) + "\n";
            if (!WriteSmallFile(statePath, text)) { error = L"Couldn't write to the drive."; return false; }
            stateWritten = true;
        }
        for (const auto& path : unflushed)
            if (!FlushPath(path)) { error = L"Couldn't write to the drive (the game files couldn't be saved)."; return false; }
        unflushed.clear();
        if (!f.Cut(target)) { error = L"Couldn't shorten " + names[k] + L" (error " + std::to_wstring(GetLastError()) + L")."; return false; }
        return true;
    };
    for (size_t k = 0; k < pkgs.size(); ++k) {
        const auto& list = jobs[k];
        if (!cut(k, std::max(floorEnd[k], list.empty() ? 0 : std::max(list[0].last, after[k][0])))) return false;
        for (size_t i = 0; i < list.size(); ++i) {
            if (cancel && *cancel) { error = L"Stopped."; return false; }
            const Item& it = plan[list[i].key];
            if (!extract(it)) return false;
            unflushed.push_back(destOf(it));
            const uint64_t target = std::max(floorEnd[k], after[k][i]);
            const uint64_t cur = pkgs[k]->file.actual;
            if (cur > target && (cur - target >= kCutStep || i + 1 == list.size()) && !cut(k, target)) return false;
        }
    }
    for (const auto& path : unflushed) FlushPath(path);
    pkgs.clear(); // close the PKGs, then delete them: nothing in them is needed any more
    for (const auto& p : paths) DeleteFileFromAppW(p.c_str());
    DeleteFileFromAppW(statePath.c_str());
    if (shrink) shrink->used = true;
    return true;
}

// Wi-Fi streaming check (2026-11-12): which PKG bytes the conversion of one PKG reads, and the peak space a convert
// while receiving would take with the PKG kept as `chunk`-sized pieces, each deleted once no file left needs it.
bool PkgLayoutReport(const std::wstring& path, uint64_t chunk, std::string& report) {
    if (!Alg().ok) return false;
    Pkg pkg;
    std::vector<Range> log;
    pkg.file.log = &log;
    std::wstring err;
    char line[512];
    if (!pkg.Open(path, err)) { report = "open failed"; return false; }
    const size_t openReads = log.size();
    uint64_t openEnd = 0, openBytes = 0;
    for (const auto& r : log) { openEnd = std::max(openEnd, r.end); openBytes += r.end - r.begin; }
    std::vector<Item> items;
    if (pkg.hasInner) {
        std::set<uint32_t> visited;
        WalkTree(pkg.inner, pkg.inner.Uroot(), "", 0, items, visited);
    }
    for (size_t i = 0; i < pkg.entries.size(); ++i) {
        const Entry& e = pkg.entries[i];
        if (e.id == 0x1 || e.id == 0x10 || e.id == 0x20 || e.id == 0x80 || e.id == 0x100 || e.id == 0x200) continue;
        items.push_back({0, true, i, e.size, "sce_sys/" + pkg.Name(e)});
    }
    struct FilePlan { uint64_t size, first, last; std::vector<Range> ranges; };
    std::vector<FilePlan> files;
    size_t split = 0;
    for (const auto& it : items) {
        FilePlan f{it.size, UINT64_MAX, 0, {}};
        if (it.sc0) {
            const Entry& e = pkg.entries[it.index];
            const uint64_t stored = e.Encrypted() ? (e.size + 15) & ~15ull : e.size;
            if (stored) f.ranges.push_back({e.offset, e.offset + stored});
        } else {
            const Inode* ino = pkg.inner.Get(it.index);
            std::vector<int64_t> blocks;
            if (!ino || !pkg.inner.Blocks(*ino, blocks)) { report = "block list failed: " + it.path; return false; }
            uint64_t left = ino->size;
            for (size_t b = 0; b < blocks.size() && left && blocks[b] > 0; ++b) {
                pkg.InnerBlockToPkg(blocks[b], f.ranges);
                left -= std::min<uint64_t>(left, kBlock);
            }
        }
        std::sort(f.ranges.begin(), f.ranges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
        std::vector<Range> merged;
        for (const auto& r : f.ranges) {
            if (!merged.empty() && r.begin <= merged.back().end) merged.back().end = std::max(merged.back().end, r.end);
            else merged.push_back(r);
        }
        f.ranges.swap(merged);
        for (const auto& r : f.ranges) { f.first = std::min(f.first, r.begin); f.last = std::max(f.last, r.end); }
        if (f.ranges.size() > 1) ++split;
        files.push_back(std::move(f));
    }
    uint64_t planEnd = 0, planBytes = 0;
    for (size_t i = 0; i < log.size(); ++i) { planEnd = std::max(planEnd, log[i].end); if (i >= openReads) planBytes += log[i].end - log[i].begin; }
    const uint64_t size = pkg.raw->Size();
    uint64_t game = 0;
    for (const auto& f : files) game += f.size;
    snprintf(line, sizeof(line), "PKG %.2f GB, game %.2f GB, %zu files (%zu in more than one piece of the PKG)\n", size / 1073741824.0, game / 1073741824.0, files.size(), split);
    report += line;
    snprintf(line, sizeof(line), "opening reads %zu (%.1f MB) up to %.1f MB; block lists read %zu more (%.1f MB) up to %.1f MB of the PKG\n",
             openReads, openBytes / 1048576.0, openEnd / 1048576.0, log.size() - openReads, planBytes / 1048576.0, planEnd / 1048576.0);
    report += line;
    // Simulation: pieces arrive in order; once the plan's bytes are in, every file whose bytes are all in is converted;
    // a piece is deleted when no file left needs it.
    const uint64_t count = (size + chunk - 1) / chunk;
    std::vector<uint32_t> refs(count, 0);
    for (const auto& f : files)
        for (const auto& r : f.ranges)
            for (uint64_t c = r.begin / chunk; c <= (r.end - 1) / chunk && c < count; ++c) ++refs[c];
    std::vector<bool> done(files.size(), false), deleted(count, false);
    std::vector<size_t> order(files.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return files[a].last < files[b].last; });
    size_t next = 0;
    uint64_t spool = 0, output = 0, peakSpool = 0, peakTotal = 0, lateFiles = 0;
    for (uint64_t c = 0; c < count; ++c) {
        const uint64_t received = std::min(size, (c + 1) * chunk);
        spool += std::min(chunk, size - c * chunk);
        peakSpool = std::max(peakSpool, spool);
        peakTotal = std::max(peakTotal, spool + output);
        if (received < planEnd) continue;
        while (next < order.size() && files[order[next]].last <= received) {
            FilePlan& f = files[order[next]];
            for (const auto& r : f.ranges)
                for (uint64_t k = r.begin / chunk; k <= (r.end - 1) / chunk && k < count; ++k) --refs[k];
            output += f.size;
            if (f.last + 4 * chunk < received) ++lateFiles;
            ++next;
        }
        for (uint64_t k = 0; k <= c; ++k)
            if (!deleted[k] && refs[k] == 0) { deleted[k] = true; spool -= std::min(chunk, size - k * chunk); }
        peakTotal = std::max(peakTotal, spool + output);
    }
    snprintf(line, sizeof(line), "pieces of %.0f MB: peak PKG pieces kept %.2f GB; peak space %.2f GB (today: PKG + game = %.2f GB)\n",
             chunk / 1048576.0, peakSpool / 1073741824.0, peakTotal / 1073741824.0, (size + game) / 1073741824.0);
    report += line;
    return true;
}

// ---- Wi-Fi: converting a PKG while it arrives (2026-11-12) ----
// The bytes arrive in order and are stored as pieces of kPiece bytes. A worker opens the PKG from the first pieces
// (header, keys, file tables: the first 4-66 MB of every PKG checked), plans the game, and converts each file once its
// bytes are in. A piece is deleted once every file that uses it is written, block by block, so the PKG never sits on
// the drive whole: TLOU 1.00 peaked at the game's 55 GB instead of 95.5 GB (PkgLayoutReport). A PKG it can't convert
// (retail, an add-on, an update without its game) is kept whole in Import, as before.
namespace {

constexpr uint64_t kPiece = 256ull << 20;

class Spool : public Source {
public:
    Spool(std::wstring dir, uint64_t size) : dir_(std::move(dir)), size_(size), state_((size + kPiece - 1) / kPiece, 0) {}
    ~Spool() override {
        for (auto& [p, h] : readers_) CloseHandle(h);
        if (writing_ != INVALID_HANDLE_VALUE) CloseHandle(writing_);
        if (keep_ != INVALID_HANDLE_VALUE) CloseHandle(keep_);
    }
    uint64_t Size() const override { return size_; }
    uint64_t Pieces() const { return state_.size(); }
    std::wstring PiecePath(uint64_t p) const { wchar_t name[32]; swprintf_s(name, L"\\%05llu.piece", (unsigned long long)p); return dir_ + name; }

    // Receiver thread. Pieces are written in order; a keep request (the worker gave up streaming) moves everything into
    // one file from then on.
    bool Append(const uint8_t* data, size_t n) {
        if (keepAsked_ && keep_ == INVALID_HANDLE_VALUE && !SwitchToKeep()) return false;
        if (keep_ != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            if (!WriteFile(keep_, data, DWORD(n), &wrote, nullptr) || wrote != n) return false;
            Advance(n);
            return true;
        }
        while (n) {
            const uint64_t at = received_.load(), piece = at / kPiece;
            if (writing_ == INVALID_HANDLE_VALUE) {
                writing_ = CreateFile2FromAppW(PiecePath(piece).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, CREATE_ALWAYS, nullptr);
                if (writing_ == INVALID_HANDLE_VALUE) return false;
                std::lock_guard<std::mutex> lock(m_);
                state_[piece] = 1;
            }
            const size_t take = (size_t)std::min<uint64_t>(n, (piece + 1) * kPiece - at);
            DWORD wrote = 0;
            if (!WriteFile(writing_, data, DWORD(take), &wrote, nullptr) || wrote != take) return false;
            data += take; n -= take;
            stored_ += take;
            if ((at + take) % kPiece == 0 || at + take == size_) {
                CloseHandle(writing_);
                writing_ = INVALID_HANDLE_VALUE;
                std::lock_guard<std::mutex> lock(m_);
                state_[piece] = 2;
            }
            Advance(take);
        }
        return true;
    }
    // Receiver: the upload ended (complete or not).
    void Close(bool complete) {
        if (writing_ != INVALID_HANDLE_VALUE) { CloseHandle(writing_); writing_ = INVALID_HANDLE_VALUE; }
        std::lock_guard<std::mutex> lock(m_);
        closed_ = true;
        complete_ = complete;
        cv_.notify_all();
    }
    // Worker: stop the conversion (an error); the receiver stops at its next piece of data.
    void Fail() { failed_ = true; std::lock_guard<std::mutex> lock(m_); cv_.notify_all(); }
    bool Failed() const { return failed_; }
    // Worker: keep the PKG whole in `path` (written as path.part, renamed by the receiver at the end).
    void AskKeep(const std::wstring& path) { keepPath_ = path; keepAsked_ = true; }
    bool KeepAsked() const { return keepAsked_; }
    const std::wstring& KeepPath() const { return keepPath_; }
    uint64_t Stored() const { return stored_.load(); }

    // Worker: waits until the bytes are in.
    bool Read(uint64_t offset, uint8_t* buf, size_t n) override {
        if (offset + n > size_) return false;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [&] { return received_.load() >= offset + n || closed_ || failed_; });
            if (received_.load() < offset + n || failed_) return false;
        }
        while (n) {
            const uint64_t piece = offset / kPiece;
            HANDLE h = Reader(piece);
            if (h == INVALID_HANDLE_VALUE) return false;
            const size_t take = (size_t)std::min<uint64_t>(n, (piece + 1) * kPiece - offset);
            OVERLAPPED at{};
            const uint64_t local = offset - piece * kPiece;
            at.Offset = DWORD(local);
            at.OffsetHigh = DWORD(local >> 32);
            DWORD got = 0;
            if (!ReadFile(h, buf, DWORD(take), &got, &at) || got != take) return false;
            buf += take; offset += take; n -= take;
        }
        return true;
    }
    // Receiver, at the end of a kept PKG: the pieces still stored go into the keep file (the worker asked after the
    // last byte), then the file is closed.
    bool FinishKeep() {
        if (keep_ == INVALID_HANDLE_VALUE && !SwitchToKeep()) return false;
        CloseHandle(keep_);
        keep_ = INVALID_HANDLE_VALUE;
        return true;
    }
    // Worker: a piece fully received?
    bool Complete(uint64_t p) { std::lock_guard<std::mutex> lock(m_); return state_[p] == 2; }
    // Worker: delete a received piece nothing needs any more.
    void Release(uint64_t p) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (state_[p] != 2) return;
            state_[p] = 3;
        }
        if (auto it = readers_.find(p); it != readers_.end()) { CloseHandle(it->second); readers_.erase(it); }
        if (DeleteFileFromAppW(PiecePath(p).c_str())) stored_ -= std::min<uint64_t>(kPiece, size_ - p * kPiece);
    }
    // Worker: waits for the end of the upload; true when it was complete.
    bool WaitClosed() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return closed_ || failed_; });
        return complete_ && !failed_;
    }
    // The receiver, once all is over: deletes the pieces left.
    void RemoveAll() {
        for (auto& [p, h] : readers_) CloseHandle(h);
        readers_.clear();
        for (uint64_t p = 0; p < state_.size(); ++p)
            if (state_[p] == 1 || state_[p] == 2) DeleteFileFromAppW(PiecePath(p).c_str());
        RemoveDirectoryFromAppW(dir_.c_str());
        stored_ = 0;
    }

private:
    void Advance(size_t n) {
        std::lock_guard<std::mutex> lock(m_);
        received_ += n;
        cv_.notify_all();
    }
    HANDLE Reader(uint64_t p) {
        if (auto it = readers_.find(p); it != readers_.end()) return it->second;
        if (readers_.size() >= 4) { CloseHandle(readers_.begin()->second); readers_.erase(readers_.begin()); }
        CREATEFILE2_EXTENDED_PARAMETERS x{sizeof(x)};
        x.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        HANDLE h = CreateFile2FromAppW(PiecePath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, &x);
        if (h != INVALID_HANDLE_VALUE) readers_[p] = h;
        return h;
    }
    // Receiver: the pieces so far go into the keep file, then deleted; later bytes go straight there.
    bool SwitchToKeep() {
        if (writing_ != INVALID_HANDLE_VALUE) { CloseHandle(writing_); writing_ = INVALID_HANDLE_VALUE; }
        const std::wstring part = keepPath_ + L".part";
        keep_ = CreateFile2FromAppW(part.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (keep_ == INVALID_HANDLE_VALUE) return false;
        std::vector<uint8_t> buf(4u << 20);
        const uint64_t have = received_.load();
        for (uint64_t p = 0; p * kPiece < have; ++p) {
            HANDLE h = CreateFile2FromAppW(PiecePath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            uint64_t left = std::min<uint64_t>(kPiece, have - p * kPiece);
            bool ok = true;
            while (ok && left) {
                DWORD got = 0, wrote = 0;
                ok = ReadFile(h, buf.data(), DWORD(std::min<uint64_t>(buf.size(), left)), &got, nullptr) && got &&
                     WriteFile(keep_, buf.data(), got, &wrote, nullptr) && wrote == got;
                left -= got;
            }
            CloseHandle(h);
            if (!ok) return false;
            DeleteFileFromAppW(PiecePath(p).c_str());
            std::lock_guard<std::mutex> lock(m_);
            state_[p] = 3;
        }
        RemoveDirectoryFromAppW(dir_.c_str());
        return true;
    }

    std::wstring dir_, keepPath_;
    uint64_t size_;
    std::vector<uint8_t> state_; // 0 not yet, 1 being written, 2 received, 3 deleted
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<uint64_t> received_{0}, stored_{0};
    std::atomic<bool> failed_{false}, keepAsked_{false};
    bool closed_ = false, complete_ = false;
    HANDLE writing_ = INVALID_HANDLE_VALUE, keep_ = INVALID_HANDLE_VALUE;
    std::map<uint64_t, HANDLE> readers_; // worker thread only
};

bool Exists(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    return GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &a) != 0;
}

} // namespace

struct PkgStream::Impl {
    Spool spool;
    std::wstring gamesRoot, keepPath;
    std::function<std::wstring(const std::wstring&)> dataFolder;
    std::function<uint64_t()> freeBytes;
    PkgStreamStatus& status;
    std::thread worker;

    Impl(const std::wstring& spoolDir, const std::wstring& keep, const std::wstring& games, uint64_t size,
         std::function<std::wstring(const std::wstring&)> folder, std::function<uint64_t()> space, PkgStreamStatus& st)
        : spool(spoolDir, size), gamesRoot(games), keepPath(keep), dataFolder(std::move(folder)), freeBytes(std::move(space)), status(st) {}

    void SetError(const std::wstring& e) {
        std::lock_guard<std::mutex> lock(status.lock);
        status.error = e;
    }
    void Keep(const std::wstring& why) {
        SetError(why);
        status.phase = 2;
        spool.AskKeep(keepPath);
    }
    void Fail(const std::wstring& why) {
        SetError(why);
        status.phase = 4;
        spool.Fail();
    }

    void Run() {
        Pkg pkg;
        std::wstring err;
        if (!pkg.OpenSource(&spool, err)) {
            if (spool.Failed()) return;
            Keep(L"Kept as a PKG: " + err);
            return;
        }
        PkgParams params;
        std::vector<uint8_t> sfo;
        const Entry* e = pkg.Find(0x1000);
        if (!e || e->size > (1u << 20) ||
            !pkg.CopyEntry(*e, [&](const uint8_t* p, size_t n) { sfo.insert(sfo.end(), p, p + n); return true; }, nullptr) ||
            !ParseSfo(sfo, params)) {
            Keep(L"Kept as a PKG: its param.sfo couldn't be read.");
            return;
        }
        std::wstring id = Wide(params.titleId.empty() && pkg.contentId.size() >= 16 ? pkg.contentId.substr(7, 9) : params.titleId);
        bool safe = id.size() == 9;
        for (const wchar_t c : id) safe = safe && iswalnum(c);
        const bool update = params.category == "gp";
        if (!safe || (params.category != "gd" && !update)) { Keep(L"Kept as a PKG: it isn't a game or an update XStation can convert."); return; }
        const std::wstring target = gamesRoot + L"\\" + id, data = target + L"\\" + dataFolder(id), marker = target + L"\\.xstation-converting";
        if (update && (!Exists(data) || Exists(marker))) { Keep(L"Kept as a PKG: send the game's own PKG first, then the update."); return; }
        if (!update && Exists(data) && !Exists(marker)) { Fail(L"This game is already in your library. Delete it first to send it again."); return; }
        const bool resume = !update && Exists(marker);
        {
            std::lock_guard<std::mutex> lock(status.lock);
            status.titleId = id;
            status.title = FromUtf8Title(params.title);
            status.target = target;
            status.update = update;
        }
        // The plan: Image0, then Sc0 into sce_sys; later wins (one PKG: the PC import tool's merge).
        std::vector<Item> items;
        if (pkg.hasInner) {
            std::set<uint32_t> visited;
            WalkTree(pkg.inner, pkg.inner.Uroot(), "", 0, items, visited);
        }
        for (size_t i = 0; i < pkg.entries.size(); ++i) {
            const Entry& en = pkg.entries[i];
            if (en.id == 0x1 || en.id == 0x10 || en.id == 0x20 || en.id == 0x80 || en.id == 0x100 || en.id == 0x200) continue;
            std::string name = pkg.Name(en);
            if (name.rfind("Sc0/", 0) == 0) name = name.substr(4);
            if (name.empty()) { char synthetic[32]; sprintf_s(synthetic, "entry_%04X.bin", en.id); name = synthetic; }
            items.push_back({0, true, i, en.size, "sce_sys/" + name});
        }
        std::map<std::string, Item> plan;
        for (auto& it : items) {
            if (!SafePath(it.path)) { Fail(L"The PKG has an unsafe file name: " + Wide(it.path)); return; }
            std::string key = it.path;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)tolower(c); });
            plan[key] = it;
        }
        // Each file's pieces, and how many files still need each piece.
        struct Work { Item item; uint64_t first; std::vector<uint64_t> pieces; };
        std::vector<Work> work;
        std::vector<uint32_t> refs(spool.Pieces(), 0);
        uint64_t total = 0, needed = 0;
        for (const auto& [key, it] : plan) {
            Work w{it, UINT64_MAX, {}};
            std::vector<Range> ranges;
            if (it.sc0) {
                const Entry& en = pkg.entries[it.index];
                const uint64_t stored = en.Encrypted() ? (en.size + 15) & ~15ull : en.size;
                if (stored) ranges.push_back({en.offset, en.offset + stored});
            } else {
                const Inode* ino = pkg.inner.Get(it.index);
                std::vector<int64_t> blocks;
                if (!ino || !pkg.inner.Blocks(*ino, blocks)) { Fail(L"Couldn't read the PKG's file table."); return; }
                uint64_t left = ino->size;
                for (size_t b = 0; b < blocks.size() && left && blocks[b] > 0; ++b) {
                    pkg.InnerBlockToPkg(blocks[b], ranges);
                    left -= std::min<uint64_t>(left, kBlock);
                }
            }
            std::set<uint64_t> pieces;
            for (const auto& r : ranges) {
                w.first = std::min(w.first, r.begin);
                for (uint64_t p = r.begin / kPiece; p <= (r.end - 1) / kPiece && p < refs.size(); ++p) pieces.insert(p);
            }
            for (uint64_t p : pieces) { ++refs[p]; w.pieces.push_back(p); }
            total += it.size;
            std::wstring dest = data + L"\\" + Wide(it.path);
            std::replace(dest.begin(), dest.end(), L'/', L'\\');
            WIN32_FILE_ATTRIBUTE_DATA a{};
            const bool there = resume && GetFileAttributesExFromAppW(dest.c_str(), GetFileExInfoStandard, &a) &&
                               ((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow) == it.size;
            if (!there) needed += it.size;
            work.push_back(std::move(w));
        }
        const uint64_t space = freeBytes ? freeBytes() : 0;
        if (space && needed + (1ull << 30) > space) {
            Fail(NotEnoughStorageText(needed + (1ull << 30), space, L"the Xbox"));
            return;
        }
        CreateDirectoryFromAppW(target.c_str(), nullptr);
        {
            std::ofstream m(marker, std::ios::trunc | std::ios::binary);
            m << "wifi\n";
            if (!m.flush()) { Fail(L"Couldn't write to the games folder."); return; }
        }
        status.progress.total = total;
        status.progress.filesTotal = (uint32_t)work.size();
        status.phase = 1;
        std::sort(work.begin(), work.end(), [](const Work& a, const Work& b) { return a.first < b.first; });
        auto sweep = [&] {
            for (uint64_t p = 0; p < refs.size(); ++p)
                if (refs[p] == 0 && spool.Complete(p)) spool.Release(p);
        };
        auto release = [&](uint64_t p) { if (refs[p] && --refs[p] == 0 && spool.Complete(p)) spool.Release(p); };
        std::vector<uint8_t> block(kBlock);
        for (auto& w : work) {
            sweep();
            const Item& it = w.item;
            std::wstring dest = data + L"\\" + Wide(it.path);
            std::replace(dest.begin(), dest.end(), L'/', L'\\');
            if (resume) {
                WIN32_FILE_ATTRIBUTE_DATA a{};
                if (GetFileAttributesExFromAppW(dest.c_str(), GetFileExInfoStandard, &a) && ((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow) == it.size) {
                    for (uint64_t p : w.pieces) release(p);
                    status.progress.done += it.size;
                    ++status.progress.files;
                    continue;
                }
            }
            if (!MakeParents(dest)) { Fail(L"Couldn't make the folder for " + Wide(it.path) + L"."); return; }
            const std::wstring part = dest + L".part";
            bool ok;
            {
                Writer out;
                if (!out.Open(part)) { Fail(L"Couldn't write " + Wide(it.path) + L"."); return; }
                if (it.sc0) {
                    ok = pkg.CopyEntry(pkg.entries[it.index], [&](const uint8_t* p, size_t n) { status.progress.done += n; return out.Write(p, n); }, nullptr);
                    for (uint64_t p : w.pieces) release(p);
                } else {
                    const Inode* ino = pkg.inner.Get(it.index);
                    std::vector<int64_t> blocks;
                    ok = ino && pkg.inner.Blocks(*ino, blocks);
                    // The last block of this file that reads each piece: the piece is free for it after that block.
                    std::map<uint64_t, size_t> lastUse;
                    std::vector<Range> ranges;
                    for (size_t b = 0; ok && b < blocks.size() && blocks[b] > 0; ++b) {
                        ranges.clear();
                        pkg.InnerBlockToPkg(blocks[b], ranges);
                        for (const auto& r : ranges)
                            for (uint64_t p = r.begin / kPiece; p <= (r.end - 1) / kPiece; ++p) lastUse[p] = b;
                    }
                    std::map<size_t, std::vector<uint64_t>> freeAfter;
                    for (const auto& [p, b] : lastUse) freeAfter[b].push_back(p);
                    uint64_t left = ino ? ino->size : 0;
                    for (size_t b = 0; ok && b < blocks.size() && left; ++b) {
                        if (blocks[b] <= 0) break;
                        if (!pkg.inner.ReadBlock(blocks[b], block.data())) { ok = false; break; }
                        const size_t n = (size_t)std::min<uint64_t>(kBlock, left);
                        if (!out.Write(block.data(), n)) { ok = false; break; }
                        left -= n;
                        status.progress.done += n;
                        if (auto f = freeAfter.find(b); f != freeAfter.end())
                            for (uint64_t p : f->second) if (std::find(w.pieces.begin(), w.pieces.end(), p) != w.pieces.end()) release(p);
                        status.stored = spool.Stored();
                    }
                    if (left) ok = false;
                    for (uint64_t p : w.pieces) if (!lastUse.count(p)) release(p);
                }
                ok = ok && out.Flush();
            }
            if (ok) DeleteFileFromAppW(dest.c_str());
            if (!ok || !MoveFileFromAppW(part.c_str(), dest.c_str())) {
                DeleteFileFromAppW(part.c_str());
                if (!spool.Failed()) Fail(L"Couldn't convert " + Wide(it.path) + L". The upload stopped, or the drive is full.");
                return;
            }
            ++status.progress.files;
            status.stored = spool.Stored();
        }
        sweep();
        // The rest of the upload (padding after the last file) still has to arrive before the PC sees "done".
        if (!spool.WaitClosed()) { Fail(L"The upload stopped before the end."); return; }
        DeleteFileFromAppW(marker.c_str());
        {
            std::ofstream size(target + L"\\xstation-game.txt", std::ios::trunc | std::ios::binary);
            size << total;
        }
        status.phase = 3;
    }

    static std::wstring FromUtf8Title(const std::string& s) {
        if (s.empty()) return L"";
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    }
};

PkgStream::PkgStream(const std::wstring& spoolDir, const std::wstring& keepPath, const std::wstring& gamesRoot, uint64_t size,
                     std::function<std::wstring(const std::wstring&)> dataFolder, std::function<uint64_t()> freeBytes, PkgStreamStatus& status)
    : impl_(std::make_unique<Impl>(spoolDir, keepPath, gamesRoot, size, std::move(dataFolder), std::move(freeBytes), status)) {
    CreateDirectoryFromAppW(spoolDir.c_str(), nullptr);
    status.phase = 0;
    impl_->worker = std::thread([this] { if (Alg().ok) impl_->Run(); else impl_->Keep(L"Kept as a PKG: Windows' crypto functions couldn't be opened."); });
}

PkgStream::~PkgStream() {
    if (impl_->worker.joinable()) { impl_->spool.Close(false); impl_->worker.join(); }
    impl_->spool.RemoveAll();
}

bool PkgStream::Append(const uint8_t* data, size_t n) {
    if (impl_->spool.Failed()) return false;
    const bool ok = impl_->spool.Append(data, n);
    impl_->status.stored = impl_->spool.Stored();
    if (!ok && !impl_->spool.Failed()) impl_->Fail(L"Couldn't store the PKG (the drive may be full).");
    return ok;
}

bool PkgStream::Finish(bool complete) {
    impl_->spool.Close(complete);
    impl_->worker.join();
    const int phase = impl_->status.phase;
    bool ok = complete && phase == 3;
    if (impl_->spool.KeepAsked()) {
        const std::wstring part = impl_->keepPath + L".part";
        ok = complete && impl_->spool.FinishKeep();
        if (ok) {
            DeleteFileFromAppW(impl_->keepPath.c_str());
            ok = MoveFileFromAppW(part.c_str(), impl_->keepPath.c_str());
        }
        if (!ok) DeleteFileFromAppW(part.c_str());
    }
    impl_->spool.RemoveAll();
    return ok;
}

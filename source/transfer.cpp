// XStation Wi-Fi transfer: a small HTTP receiver. SPDX-License-Identifier: GPL-2.0-or-later
// Listens only after the Add Game page is opened and only in the dashboard (never while a game runs).
// Every upload needs the pairing code shown on screen; files can only land under Games\ or Import\.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "transfer.h"
#include "game_prepare.h"
#include "pkg_extract.h"
#include <cwctype>

namespace {

std::wstring root;
std::string code;
std::atomic<bool> running{false};
std::atomic<uint64_t> received{0};
std::atomic<int> active{0}, wrongCodes{0}, finished{0};
std::mutex nameLock;
std::wstring currentName;
// Wi-Fi PKGs (2026-11-12): converted while they arrive (pkg_extract.h PkgStream), one at a time.
PkgStreamStatus streamStatus;
std::atomic<bool> streaming{false};
std::atomic<int> conversions{0};
std::mutex convertedLock;
std::wstring convertedId, convertedTarget, lastPkgMessage;
bool convertedUpdate = false;
// 2026-11-14: why the last Wi-Fi PKG stopped (empty while one runs or after a good one). A refusal reaches the
// browser while it is still sending, so it sees only "connection lost"; the page asks /pkgresult for the reason.
std::wstring lastPkgError;

const char kPage[] = R"HTML(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>XStation transfer</title>
<style>
:root{color-scheme:dark}body{margin:0;background:#0c1424;color:#fff;font:16px/1.5 "Segoe UI",system-ui,sans-serif}
main{max-width:640px;margin:0 auto;padding:28px 16px}h1{font-size:24px;letter-spacing:.14em;margin:0 0 20px}
section{background:#16213a;border:1px solid #2a3858;border-radius:10px;padding:16px;margin:14px 0}h2{font-size:17px;margin:0 0 4px}p{margin:0 0 10px;color:#b9c4d8;font-size:14px}
label{display:block;font-size:14px;margin-bottom:6px}input[type=text]{font:22px monospace;letter-spacing:.2em;width:9ch;background:#0c1424;color:#fff;border:1px solid #47587e;border-radius:6px;padding:6px 10px}
input[type=file]{display:block;max-width:100%;margin-bottom:10px}button{font:600 15px "Segoe UI",sans-serif;background:#fff;color:#0c1424;border:0;border-radius:6px;padding:9px 18px;cursor:pointer}button:disabled{opacity:.5}
progress{width:100%;height:14px;margin-top:6px}#status{font-size:14px;word-break:break-word;min-height:42px;white-space:pre-line}
</style><main><h1>XSTATION</h1>
<section><label for="code">Pairing code shown on the Xbox</label><input id="code" type="text" inputmode="numeric" maxlength="6" autocomplete="off"></section>
<section><h2>Prepared game folder</h2><p>Choose the folder the PC import tool made (named like CUSA07408). It shows up under Games after you restart XStation.</p><input id="dir" type="file" webkitdirectory><button id="sendDir">Send folder</button></section>
<section><h2>PKG file</h2><p>The Xbox converts it while it arrives, so it only needs room for the game, not the PKG as well. Send the game's PKG and its update together: they go one at a time, the bigger (the game) first.</p><input id="pkg" type="file" accept=".pkg" multiple><button id="sendPkg">Send PKG</button></section>
<section><div id="status">Nothing sent yet.</div><progress id="bar" value="0" max="1"></progress></section></main>
<script>
const $=id=>document.getElementById(id);let busy=false;
function fmt(n){return n>=1073741824?(n/1073741824).toFixed(2)+" GB":(n/1048576).toFixed(1)+" MB"}
// 2026-11-14: the PKG's title ID and category (header + param.sfo), so the Xbox can say no before anything is sent.
async function pkgInfo(f){try{const h=new DataView(await f.slice(0,4096).arrayBuffer());if(h.byteLength<0x64||h.getUint32(0)!=0x7F434E54)return null;
 const n=h.getUint32(0x10),t=h.getUint32(0x18);if(n>100000)return null;const tb=new DataView(await f.slice(t,t+n*32).arrayBuffer());let o=-1,z=0;
 for(let i=0;i*32+32<=tb.byteLength;i++)if(tb.getUint32(i*32)==0x1000){o=tb.getUint32(i*32+16);z=tb.getUint32(i*32+20);break}
 const info={id:new TextDecoder().decode(new Uint8Array(h.buffer,0x40,36)).substr(7,9),cat:"",title:""};if(o<0||z<20||z>1048576)return info;
 const s=new DataView(await f.slice(o,o+z).arrayBuffer());if(s.byteLength<20||s.getUint32(0)!=0x00505346)return info;z=s.byteLength;
 const str=(p,l)=>{const a=new Uint8Array(s.buffer,p,Math.min(l,z-p));const e=a.indexOf(0);return new TextDecoder().decode(e<0?a:a.subarray(0,e))};
 const k=s.getUint32(8,true),v=s.getUint32(12,true),c=s.getUint32(16,true);
 for(let i=0;i<c&&20+i*16+16<=z;i++){const e=20+i*16,key=k+s.getUint16(e,true),fm=s.getUint16(e+2,true),len=s.getUint32(e+4,true),val=v+s.getUint32(e+12,true);
  if(key>=z||fm!=0x0204||val+len>z)continue;const name=str(key,z),text=str(val,len);if(name=="CATEGORY")info.cat=text;else if(name=="TITLE_ID"&&text)info.id=text;else if(name=="TITLE")info.title=text}
 return info}catch(e){return null}}
async function send(list,lanes=4){ if(busy)return; const code=$("code").value.trim(); if(!/^\d{6}$/.test(code)){$("status").textContent="Enter the 6-digit pairing code first.";return}
 if(!list.length){$("status").textContent="Choose something to send first.";return}
 const checked=await fetch("/check",{headers:{"X-Pair-Code":code}}).catch(()=>null);if(!checked||checked.status!=204){$("status").textContent=checked?"Wrong pairing code.":"Can't reach the Xbox. Is XStation open on the Add Game page?";return}
 for(const x of list){if(!x.path.startsWith("Import/"))continue;$("status").textContent="Checking "+x.file.name+"\u2026";const i=await pkgInfo(x.file);if(!i||!i.cat)continue;
  const r=await fetch("/pkgcheck?id="+encodeURIComponent(i.id)+"&cat="+encodeURIComponent(i.cat),{headers:{"X-Pair-Code":code}}).catch(()=>null);
  if(r&&r.status==409){$("status").textContent=(i.title?i.title+": ":"")+await r.text();return}}
 const top=list[0].path.split("/").slice(0,2).join("/");if(top.startsWith("Games/")){$("status").textContent="Checking free space…";
  const r=await fetch("/spacecheck?bytes="+list.reduce((a,x)=>a+x.file.size,0)+"&folder="+encodeURIComponent(top),{headers:{"X-Pair-Code":code}}).catch(()=>null);
  if(r&&r.status==507){$("status").textContent=await r.text();return}}
 busy=true;$("sendDir").disabled=$("sendPkg").disabled=true;const total=list.reduce((a,x)=>a+x.file.size,0);let done=0,next=0,fail=null,count=0;const live=new Map();
 const paint=(n)=>{const now=done+[...live.values()].reduce((a,b)=>a+b,0);$("bar").value=total?now/total:1;$("status").textContent=fmt(now)+" of "+fmt(total)+" · "+count+" of "+list.length+" files · "+n};
 const one=x=>new Promise((res,rej)=>{const r=new XMLHttpRequest();r.open("PUT","/upload?path="+encodeURIComponent(x.path));r.setRequestHeader("X-Pair-Code",code);
  r.upload.onprogress=e=>{live.set(x.path,e.loaded);paint(x.path)};r.onload=()=>{live.delete(x.path);r.status<300?res():rej(new Error(r.status==403?"Wrong pairing code.":r.status==507?(r.responseText&&!r.responseText.startsWith("507")?r.responseText:"Not enough storage on the Xbox. Free up space and try again."):r.status==409&&r.responseText?r.responseText:"Upload failed ("+r.status+"): "+x.path))};r.onerror=async()=>{live.delete(x.path);let m="";if(x.path.startsWith("Import/")){const q=await fetch("/pkgresult",{headers:{"X-Pair-Code":code}}).catch(()=>null);if(q&&q.status==409)m=await q.text()}rej(new Error(m||"Connection lost at "+x.path))};r.send(x.file)});
 const worker=async()=>{while(!fail&&next<list.length){const x=list[next++];try{await one(x);done+=x.file.size;count++;paint(x.path)}catch(e){fail=e}}};
 await Promise.all(Array.from({length:lanes},worker));
 $("status").textContent=fail?fail.message:"Done: "+count+" files, "+fmt(total)+". Restart XStation to see new games.";busy=false;$("sendDir").disabled=$("sendPkg").disabled=false}
$("sendDir").onclick=()=>send([...$("dir").files].map(f=>({file:f,path:"Games/"+f.webkitRelativePath})));
$("sendPkg").onclick=()=>send([...$("pkg").files].sort((a,b)=>b.size-a.size).map(f=>({file:f,path:"Import/"+f.name})),1);
</script></html>)HTML";

void SendAll(SOCKET s, const char* data, size_t size) {
    while (size) {
        const int n = send(s, data, size > 65536 ? 65536 : (int)size, 0);
        if (n <= 0) return;
        data += n;
        size -= (size_t)n;
    }
}

void Reply(SOCKET s, const char* status, const char* type, const char* body, size_t size) {
    char head[256];
    const int n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, size);
    SendAll(s, head, (size_t)n);
    SendAll(s, body, size);
}

void Plain(SOCKET s, const char* status) { Reply(s, status, "text/plain", status, strlen(status)); }
void Message(SOCKET s, const char* status, const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string body((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), body.data(), n, nullptr, nullptr);
    Reply(s, status, "text/plain; charset=utf-8", body.data(), body.size());
}

bool EndsWithPkg(const std::wstring& path) {
    if (path.size() < 4) return false;
    std::wstring tail = path.substr(path.size() - 4);
    for (wchar_t& c : tail) c = (wchar_t)towlower(c);
    return tail == L".pkg";
}


std::string Header(const std::string& head, const char* name) {
    std::string lower = head;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    const std::string key = std::string("\r\n") + name + ":";
    const size_t at = lower.find(key);
    if (at == std::string::npos) return {};
    size_t begin = at + key.size();
    const size_t end = head.find("\r\n", begin);
    while (begin < end && head[begin] == ' ') ++begin;
    return head.substr(begin, end - begin);
}

std::string Decode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += in[i] == '+' ? ' ' : in[i];
    }
    return out;
}

// Relative to the data root: starts with Games/ or Import/; no "..", no drive, no empty parts.
bool SafePath(const std::string& path, std::wstring& out) {
    if (path.rfind("Games/", 0) && path.rfind("Import/", 0)) return false;
    if (path.size() > 900 || path.find('\\') != std::string::npos || path.find(':') != std::string::npos) return false;
    size_t at = 0;
    while (at <= path.size()) {
        const size_t next = path.find('/', at);
        const std::string part = path.substr(at, next == std::string::npos ? std::string::npos : next - at);
        if (part.empty() || part == "." || part == "..") return false;
        if (next == std::string::npos) break;
        at = next + 1;
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), (int)path.size(), nullptr, 0);
    if (n <= 0) return false;
    out.resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), out.data(), n);
    for (wchar_t& c : out) if (c == L'/') c = L'\\';
    return true;
}

void MakeParents(const std::wstring& file) {
    for (size_t at = root.size() + 1; (at = file.find(L'\\', at)) != std::wstring::npos; ++at)
        CreateDirectoryW(file.substr(0, at).c_str(), nullptr);
}

// A PKG under Import\: converted while it arrives. Returns after the reply was sent.
void UploadPkg(SOCKET s, const std::wstring& relative, uint64_t length, const char* extra, size_t extraSize) {
    const std::wstring file = root + L"\\" + relative, name = relative.substr(relative.find_last_of(L'\\') + 1);
    MakeParents(file);
    CreateDirectoryW((root + L"\\Games").c_str(), nullptr);
    {
        std::lock_guard<std::mutex> lock(nameLock);
        currentName = relative;
    }
    {
        std::lock_guard<std::mutex> lock(streamStatus.lock);
        streamStatus.error.clear();
        streamStatus.titleId.clear();
        streamStatus.title.clear();
        streamStatus.target.clear();
        streamStatus.update = false;
    }
    {
        std::lock_guard<std::mutex> lock(convertedLock);
        lastPkgError.clear();
    }
    streamStatus.stored = 0;
    streamStatus.progress.done = streamStatus.progress.total = 0;
    streamStatus.progress.files = streamStatus.progress.filesTotal = 0;
    ++active;
    bool ok = true, complete = false;
    {
        PkgStream stream(file + L".stream", file, root + L"\\Games", length, [](const std::wstring& id) { return GamePrepareDataFolder(id); },
                         [] { ULARGE_INTEGER f{}; GetDiskFreeSpaceExW(root.c_str(), &f, nullptr, nullptr); return (uint64_t)f.QuadPart; }, streamStatus);
        uint64_t left = length;
        if (extraSize) {
            const size_t take = extraSize > left ? (size_t)left : extraSize;
            ok = stream.Append(reinterpret_cast<const uint8_t*>(extra), take);
            left -= take;
            received += take;
        }
        std::vector<char> buffer(1 << 20);
        while (ok && left) {
            const int n = recv(s, buffer.data(), left > buffer.size() ? (int)buffer.size() : (int)left, 0);
            if (n <= 0) { ok = false; break; }
            ok = stream.Append(reinterpret_cast<const uint8_t*>(buffer.data()), (size_t)n);
            left -= (uint64_t)n;
            received += (uint64_t)n;
        }
        complete = ok && !left;
        ok = stream.Finish(complete);
    }
    --active;
    std::wstring error, id, target;
    bool update;
    {
        std::lock_guard<std::mutex> lock(streamStatus.lock);
        error = streamStatus.error; id = streamStatus.titleId; target = streamStatus.target; update = streamStatus.update;
    }
    const int phase = streamStatus.phase;
    {
        std::lock_guard<std::mutex> lock(convertedLock);
        lastPkgMessage = ok ? (phase == 3 ? L"Converted " + id + (update ? L" (update)" : L"") : error) : error;
        if (!ok) lastPkgError = error.empty() ? (complete ? L"The PKG couldn't be converted." : L"The upload stopped before the end. Check the Wi-Fi and send it again.") : error;
    }
    if (ok && phase == 3) {
        {
            std::lock_guard<std::mutex> lock(convertedLock);
            convertedId = id; convertedTarget = target; convertedUpdate = update;
        }
        ++conversions;
    }
    if (ok) { ++finished; Plain(s, "204 No Content"); return; }
    if (!complete && error.empty()) { Plain(s, "500 Internal Server Error"); return; }
    Message(s, "409 Conflict", error.empty() ? L"The PKG couldn't be converted." : error);
}

bool Paired(SOCKET s, const std::string& head) {
    if (wrongCodes.load() >= 20) { Plain(s, "403 Forbidden"); return false; }
    if (Header(head, "x-pair-code") != code) { ++wrongCodes; Plain(s, "403 Forbidden"); return false; }
    return true;
}

std::string Query(const std::string& target, const char* name) {
    const std::string key = std::string(name) + "=";
    for (size_t at = target.find('?'); at != std::string::npos; at = target.find('&', at + 1))
        if (target.compare(at + 1, key.size(), key) == 0) {
            const size_t begin = at + 1 + key.size(), end = target.find('&', begin);
            return Decode(target.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        }
    return {};
}

// 2026-11-14: GET /pkgcheck?id=CUSA00900&cat=gd, before a PKG is sent: the refusal PkgStream would give once the
// PKG starts arriving (409 + reason), else 204.
void PkgCheck(SOCKET s, const std::string& head, const std::string& target) {
    if (!Paired(s, head)) return;
    const std::string id = Query(target, "id"), cat = Query(target, "cat");
    bool safe = id.size() == 9;
    for (const char c : id) safe = safe && isalnum((unsigned char)c);
    if (streaming) { Message(s, "409 Conflict", L"Another PKG is being converted. Send PKGs one at a time."); return; }
    if (safe && cat == "gd") {
        const std::wstring wid(id.begin(), id.end()), folder = root + L"\\Games\\" + wid;
        const bool data = GetFileAttributesW((folder + L"\\" + GamePrepareDataFolder(wid)).c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool resuming = GetFileAttributesW((folder + L"\\.xstation-converting").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (data && !resuming) {
            Message(s, "409 Conflict", L"This game is already in your library. To send it again, delete it in XStation first (your saves are kept).");
            return;
        }
    }
    Plain(s, "204 No Content");
}

uint64_t FolderBytes(const std::wstring& folder, int depth = 0) {
    if (depth > 32) return 0;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t sum = 0;
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..") || (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) sum += FolderBytes(folder + L"\\" + f.cFileName, depth + 1);
        else sum += (uint64_t(f.nFileSizeHigh) << 32) | f.nFileSizeLow;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return sum;
}

// 2026-11-18: GET
// /spacecheck?bytes=<total>&folder=Games/<name>, before a game folder is sent: 507 + the not-enough-storage message when
// it doesn't fit, else 204. Files already in that folder count as room (sending again replaces them).
void SpaceCheck(SOCKET s, const std::string& head, const std::string& target) {
    if (!Paired(s, head)) return;
    const uint64_t bytes = _strtoui64(Query(target, "bytes").c_str(), nullptr, 10);
    const std::string name = Query(target, "folder");
    std::wstring folder;
    const uint64_t present = name.rfind("Games/", 0) == 0 && SafePath(name, folder) ? FolderBytes(root + L"\\" + folder) : 0;
    const uint64_t need = (bytes > present ? bytes - present : 0) + (512ull << 20);
    ULARGE_INTEGER freeBytes{};
    if (bytes && GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr) && need > freeBytes.QuadPart) {
        Message(s, "507 Insufficient Storage", NotEnoughStorageText(need, freeBytes.QuadPart, L"the Xbox"));
        return;
    }
    Plain(s, "204 No Content");
}

void Upload(SOCKET s, const std::string& head, const std::string& target, const char* extra, size_t extraSize) {
    if (wrongCodes.load() >= 20) { Plain(s, "403 Forbidden"); return; }
    if (Header(head, "x-pair-code") != code) { ++wrongCodes; Plain(s, "403 Forbidden"); return; }
    const size_t q = target.find("?path=");
    std::wstring relative;
    if (q == std::string::npos || !SafePath(Decode(target.substr(q + 6)), relative)) { Plain(s, "400 Bad Request"); return; }
    const std::string lengthText = Header(head, "content-length");
    if (lengthText.empty()) { Plain(s, "411 Length Required"); return; }
    const uint64_t length = _strtoui64(lengthText.c_str(), nullptr, 10);
    // Wi-Fi PKGs convert while they arrive (2026-11-12; a file Import\stream-off.txt turns it off).
    if (relative.rfind(L"Import\\", 0) == 0 && EndsWithPkg(relative) && relative.find(L'\\', 7) == std::wstring::npos &&
        GetFileAttributesW((root + L"\\Import\\stream-off.txt").c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (streaming.exchange(true)) { Message(s, "409 Conflict", L"Another PKG is being converted. Send PKGs one at a time."); return; }
        ULARGE_INTEGER space{};
        if (GetDiskFreeSpaceExW(root.c_str(), &space, nullptr, nullptr) && space.QuadPart < (2ull << 30)) {
            streaming = false;
            // The game's size is known only once the PKG's file table arrives (PkgStream); under 2 GB nothing fits.
            wchar_t text[300];
            swprintf_s(text, L"Not enough storage. The Xbox has only %.1f GB free, and a game needs much more (about as much as its PKG, "
                             L"often more). Free up space (delete games or files you don't need) and try again.",
                       space.QuadPart / 1073741824.0);
            Message(s, "507 Insufficient Storage", text);
            return;
        }
        UploadPkg(s, relative, length, extra, extraSize);
        streaming = false;
        return;
    }
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < length + (64ull << 20)) {
        Message(s, "507 Insufficient Storage", NotEnoughStorageText(length + (64ull << 20), freeBytes.QuadPart, L"the Xbox",
                                                                     L"Not enough storage for " + relative + L"."));
        return;
    }
    const std::wstring file = root + L"\\" + relative, part = file + L".part";
    MakeParents(file);
    HANDLE h = CreateFile2(part.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Plain(s, "500 Internal Server Error"); return; }
    {
        std::lock_guard<std::mutex> lock(nameLock);
        currentName = relative;
    }
    ++active;
    uint64_t left = length;
    DWORD wrote;
    bool ok = true;
    if (extraSize) {
        const size_t take = extraSize > left ? (size_t)left : extraSize;
        ok = WriteFile(h, extra, (DWORD)take, &wrote, nullptr) && wrote == take;
        left -= take;
        received += take;
    }
    std::vector<char> buffer(1 << 20);
    while (ok && left) {
        const int n = recv(s, buffer.data(), left > buffer.size() ? (int)buffer.size() : (int)left, 0);
        if (n <= 0) { ok = false; break; }
        ok = WriteFile(h, buffer.data(), (DWORD)n, &wrote, nullptr) && wrote == (DWORD)n;
        left -= (uint64_t)n;
        received += (uint64_t)n;
    }
    CloseHandle(h);
    --active;
    if (!ok) { DeleteFileW(part.c_str()); Plain(s, "500 Internal Server Error"); return; }
    if (!MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(part.c_str()); Plain(s, "500 Internal Server Error"); return; }
    ++finished;
    Plain(s, "204 No Content");
}

void Connection(SOCKET s) {
    std::string data;
    char chunk[8192];
    size_t end;
    while ((end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() > 32768) { closesocket(s); return; }
        const int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) { closesocket(s); return; }
        data.append(chunk, (size_t)n);
    }
    const std::string head = data.substr(0, end + 2);
    const size_t a = head.find(' '), b = head.find(' ', a + 1);
    if (a == std::string::npos || b == std::string::npos) { closesocket(s); return; }
    const std::string method = head.substr(0, a), target = head.substr(a + 1, b - a - 1);
    if (method == "GET" && (target == "/" || target == "/index.html")) Reply(s, "200 OK", "text/html; charset=utf-8", kPage, sizeof(kPage) - 1);
    else if (method == "GET" && target == "/check") {
        if (wrongCodes.load() >= 20) Plain(s, "403 Forbidden");
        else if (Header(head, "x-pair-code") == code) Plain(s, "204 No Content");
        else { ++wrongCodes; Plain(s, "403 Forbidden"); }
    }
    else if (method == "GET" && target.rfind("/pkgcheck?", 0) == 0) PkgCheck(s, head, target);
    else if (method == "GET" && target.rfind("/spacecheck?", 0) == 0) SpaceCheck(s, head, target);
    else if (method == "GET" && target == "/pkgresult") {
        if (Paired(s, head)) {
            std::wstring why;
            { std::lock_guard<std::mutex> lock(convertedLock); why = lastPkgError; }
            if (why.empty() || streaming) Plain(s, "204 No Content"); else Message(s, "409 Conflict", why);
        }
    }
    else if (method == "PUT" && target.rfind("/upload", 0) == 0) Upload(s, head, target, data.data() + end + 4, data.size() - end - 4);
    else Plain(s, "404 Not Found");
    // Read what the sender still has in flight (a rejected upload), or it sees a reset and not the answer.
    shutdown(s, SD_SEND);
    DWORD wait = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));
    uint64_t drained = 0;
    std::vector<char> sink(1 << 16);
    for (int n; drained < (256ull << 20) && (n = recv(s, sink.data(), (int)sink.size(), 0)) > 0;) drained += (uint64_t)n;
    closesocket(s);
}

} // namespace

bool StartTransferServer(const std::wstring& dataRoot, std::string& pairingCode, int port) {
    if (running.exchange(true)) { pairingCode = code; return true; }
    root = dataRoot;
    unsigned char random[4]{};
    BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    uint32_t value;
    memcpy(&value, random, 4);
    char text[8];
    snprintf(text, sizeof(text), "%06u", value % 1000000);
    code = text;
    pairingCode = code;
    WSADATA wsa;
    auto note = [&](const char* step, int error) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, (dataRoot + L"\\xstation-wifi.log").c_str(), L"a") && f) { fprintf(f, "%s: %d\n", step, error); fclose(f); }
    };
    if (const int e = WSAStartup(MAKEWORD(2, 2), &wsa)) { note("WSAStartup", e); running = false; return false; }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { note("socket", WSAGetLastError()); running = false; return false; }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
#ifdef XSTATION_TRANSFER_LOOPBACK
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // PC test tool (tools/pkgtest xfertest): this PC only, no firewall prompt
#else
    address.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
    if (bind(listener, (sockaddr*)&address, sizeof(address))) { note("bind", WSAGetLastError()); closesocket(listener); running = false; return false; }
    if (listen(listener, 16)) { note("listen", WSAGetLastError()); closesocket(listener); running = false; return false; }
    note("listening", port);
    std::thread([listener] {
        for (;;) {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            std::thread(Connection, client).detach();
        }
    }).detach();
    return true;
}

TransferState GetTransferState() {
    TransferState state;
    state.received = received.load();
    state.active = active.load();
    state.finished = finished.load();
    state.streaming = streaming.load();
    state.streamPhase = streamStatus.phase.load();
    state.streamStored = streamStatus.stored.load();
    state.convertDone = streamStatus.progress.done.load();
    state.convertTotal = streamStatus.progress.total.load();
    state.conversions = conversions.load();
    {
        std::lock_guard<std::mutex> lock(streamStatus.lock);
        state.streamTitle = streamStatus.title;
    }
    {
        std::lock_guard<std::mutex> lock(convertedLock);
        state.convertedId = convertedId;
        state.convertedTarget = convertedTarget;
        state.convertedUpdate = convertedUpdate;
        state.lastPkgMessage = lastPkgMessage;
    }
    std::lock_guard<std::mutex> lock(nameLock);
    state.name = currentName;
    return state;
}

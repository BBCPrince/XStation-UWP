// XStation Wi-Fi transfer: a small HTTP receiver. SPDX-License-Identifier: GPL-2.0-or-later
// Listens only after the Add Game page is opened and only in the dashboard (never while a game runs).
// Every upload needs the pairing code shown on screen; files can only land under Games\ or Import\.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
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
// 2026-11-19: the Engines folder, for the compatibility check before a Wi-Fi send (SetTransferEngines).
std::wstring enginesRoot;

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
<section><h2>Prepared game folder</h2><p>Choose a prepared game folder (named like CUSA07408, with GameData and GameBoot inside). It shows up under Games after you restart XStation.</p><input id="dir" type="file" webkitdirectory><button id="sendDir">Send folder</button></section>
<section><h2>PKG file</h2><p>The Xbox converts it while it arrives, so it only needs room for the game, not the PKG as well. Send the game's PKG and its update together: they go one at a time, the bigger (the game) first. Bloodborne (update 1.09) and God of War (update 1.35) need both chosen at once; the other games need only the game PKG, no update. First the Xbox reads a few MB of the game's files to check that XStation can play this copy (a copy from any country with the same game files works); nothing else is sent if it can't.</p><input id="pkg" type="file" accept=".pkg" multiple><button id="sendPkg">Send PKG</button></section>
<section><div id="status">Nothing sent yet.</div><progress id="bar" value="0" max="1"></progress></section></main>
<script>
const $=id=>document.getElementById(id);let busy=false;
function fmt(n){return n>=1073741824?(n/1073741824).toFixed(2)+" GB":(n/1048576).toFixed(1)+" MB"}
// 2026-11-14: the PKG's title ID and category (header + param.sfo), so the Xbox can say no before anything is sent.
async function pkgInfo(f){try{const h=new DataView(await f.slice(0,4096).arrayBuffer());if(h.byteLength<0x64||h.getUint32(0)!=0x7F434E54)return null;
 const n=h.getUint32(0x10),t=h.getUint32(0x18);if(n>100000)return null;const tb=new DataView(await f.slice(t,t+n*32).arrayBuffer());let o=-1,z=0;
 for(let i=0;i*32+32<=tb.byteLength;i++)if(tb.getUint32(i*32)==0x1000){o=tb.getUint32(i*32+16);z=tb.getUint32(i*32+20);break}
 const info={id:new TextDecoder().decode(new Uint8Array(h.buffer,0x40,36)).substr(7,9),cat:"",title:"",ver:""};if(o<0||z<20||z>1048576)return info;
 const s=new DataView(await f.slice(o,o+z).arrayBuffer());if(s.byteLength<20||s.getUint32(0)!=0x00505346)return info;z=s.byteLength;
 const str=(p,l)=>{const a=new Uint8Array(s.buffer,p,Math.min(l,z-p));const e=a.indexOf(0);return new TextDecoder().decode(e<0?a:a.subarray(0,e))};
 const k=s.getUint32(8,true),v=s.getUint32(12,true),c=s.getUint32(16,true);
 for(let i=0;i<c&&20+i*16+16<=z;i++){const e=20+i*16,key=k+s.getUint16(e,true),fm=s.getUint16(e+2,true),len=s.getUint32(e+4,true),val=v+s.getUint32(e+12,true);
  if(key>=z||fm!=0x0204||val+len>z)continue;const name=str(key,z),text=str(val,len);if(name=="CATEGORY")info.cat=text;else if(name=="TITLE_ID"&&text)info.id=text;else if(name=="TITLE")info.title=text;else if(name=="APP_VER")info.ver=text}
 return info}catch(e){return null}}
// 2026-11-19: the Xbox checks the game's own files before the send: it asks for the PKG bytes it needs (tables, eboot.bin,
// modules, param.sfo) and this page sends just those slices. Empty when it's compatible, else why it wasn't sent.
async function probe(code,i,upd){const files=[i.file].concat(upd?[upd]:[]),h={"X-Pair-Code":code};let read=0;
 const say=()=>$("status").textContent="Checking that "+(i.title||i.id)+" is compatible… ("+fmt(read)+" of its files read)";say();
 let r=await fetch("/probe/start?id="+encodeURIComponent(i.id)+"&sizes="+files.map(f=>f.size).join(","),{method:"POST",headers:h}).catch(()=>null);
 for(;;){if(!r)return"Lost the connection to the Xbox while checking the PKG. Send it again.";const t=await r.text();
  if(r.status==409)return t;if(r.status!=200)return t||"The Xbox couldn't check the PKG ("+r.status+"). Send it again.";
  const p=t.split(" ");if(p[0]=="ok")return"";if(p[0]!="need")return"The Xbox gave an answer this page doesn't know: "+t;
  const f=files[+p[1]],o=+p[2],l=+p[3];read+=l;say();
  r=await fetch("/probe/data?offset="+o,{method:"PUT",headers:h,body:await f.slice(o,o+l).arrayBuffer()}).catch(()=>null)}}
async function send(list,lanes=4){ if(busy)return; const code=$("code").value.trim(); if(!/^\d{6}$/.test(code)){$("status").textContent="Enter the 6-digit pairing code first.";return}
 if(!list.length){$("status").textContent="Choose something to send first.";return}
 const checked=await fetch("/check",{headers:{"X-Pair-Code":code}}).catch(()=>null);if(!checked||checked.status!=204){$("status").textContent=checked?"Wrong pairing code.":"Can't reach the Xbox. Is XStation open on the Add Game page?";return}
 const infos=[];for(const x of list){if(!x.path.startsWith("Import/"))continue;$("status").textContent="Checking "+x.file.name+"\u2026";const i=await pkgInfo(x.file);if(i&&i.cat){i.file=x.file;infos.push(i)}}
 for(const i of infos){const game=infos.some(o=>o.id==i.id&&o.cat=="gd"),upd=infos.filter(o=>o.id==i.id&&o.cat=="gp").map(o=>o.ver).join(",");
  const r=await fetch("/pkgcheck?id="+encodeURIComponent(i.id)+"&cat="+encodeURIComponent(i.cat)+"&ver="+encodeURIComponent(i.ver)+"&withgame="+(game?1:0)+"&withupdate="+encodeURIComponent(upd)+"&probe=1",{headers:{"X-Pair-Code":code}}).catch(()=>null);
  if(r&&r.status==409){$("status").textContent=(i.title?i.title+": ":"")+await r.text();return}}
 for(const i of infos){if(i.cat!="gd")continue;const u=infos.filter(o=>o.id==i.id&&o.cat=="gp").sort((a,b)=>a.ver<b.ver?1:-1)[0];
  const m=await probe(code,i,u&&u.file);if(m){$("status").textContent=(i.title?i.title+": ":"")+m;return}}
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
    if (safe && (cat == "gd" || cat == "gp")) {
        const std::wstring wid(id.begin(), id.end());
        const std::string ver = Query(target, "ver"), withUpdate = "," + Query(target, "withupdate") + ",";
        const bool withGame = Query(target, "withgame") == "1";
        const std::wstring needed = GameNeededUpdate(wid);
        const std::string need(needed.begin(), needed.end());
        auto shortVer = [](std::string v) { while (v.size() > 1 && v[0] == '0' && v[1] != '.') v.erase(0, 1); return std::wstring(v.begin(), v.end()); };
        const std::wstring folder = root + L"\\Games\\" + wid;
        const bool gameThere = GetFileAttributesW((folder + L"\\" + GamePrepareDataFolder(wid)).c_str()) != INVALID_FILE_ATTRIBUTES &&
                               GetFileAttributesW((folder + L"\\.xstation-converting").c_str()) == INVALID_FILE_ATTRIBUTES;
        if (!GamePrepareSupported(GameEngineId(wid)) && Query(target, "probe") == "1") {
            if (cat == "gp" && !withGame && !gameThere) {
                Message(s, "409 Conflict", L"Choose the game PKG and this update PKG together (both in the file picker), then send: XStation checks "
                                           L"them together.");
                return;
            }
            Plain(s, "204 No Content");
            return;
        }
        if (!GamePrepareSupported(GameEngineId(wid))) {
            Message(s, "409 Conflict", L"Not compatible: XStation has no engine for this game (" + wid + L"), or this region of it isn't "
                                       L"known to work, so it wasn't sent. If it's another region of a game on the Compatible tab, put the "
                                       L"PKG on a USB drive: XStation checks the game itself there before converting.");
            return;
        }
        if (cat == "gd" && !ver.empty() && ver != "01.00" && ver != need) {
            Message(s, "409 Conflict", L"Not compatible: this copy is version " + shortVer(ver) + L", but XStation plays this game as 1.00" +
                                       (need.empty() ? std::wstring(L" with no update") : L" with update " + shortVer(need)) +
                                       L", so it wasn't sent.");
            return;
        }
        if (cat == "gd" && !need.empty() && ver != need && withUpdate.find("," + need + ",") == std::string::npos) {
            Message(s, "409 Conflict", L"This game plays with its update " + shortVer(need) + L". Choose the game PKG and the update " + shortVer(need) +
                                       L" PKG together (both in the file picker), then send: they convert together.");
            return;
        }
        if (cat == "gp" && need.empty() && GamePrepareSupported(GameEngineId(wid))) {
            Message(s, "409 Conflict", L"XStation plays this game with no update, so this update isn't needed (it wouldn't play). Send only the game PKG.");
            return;
        }
        if (cat == "gp" && !need.empty() && ver != need) {
            Message(s, "409 Conflict", L"This update is " + shortVer(ver) + L", but the game needs update " + shortVer(need) + L". Send that update's PKG instead.");
            return;
        }
        if (cat == "gp" && !need.empty() && !withGame && !gameThere) {
            Message(s, "409 Conflict", L"Choose the game PKG and this update PKG together (both in the file picker), then send: they convert together.");
            return;
        }
    }
    Plain(s, "204 No Content");
}

struct Probe {
    std::mutex lock;
    std::condition_variable cv;
    std::wstring id;
    std::vector<uint64_t> sizes;
    bool asking = false, answered = false, cancel = false, done = false;
    int askPkg = 0;
    uint64_t askOffset = 0, askLength = 0;
    std::vector<uint8_t> answer;
    int result = -1; // 1 compatible, 0 not, -1 couldn't check
    std::wstring engine, error;
    uint64_t fetched = 0;
    int trips = 0;
};
std::mutex probeLock;
std::shared_ptr<Probe> currentProbe;
std::atomic<int> probeCount{0};

void WifiLog(const std::string& line) {
    FILE* f = nullptr;
    if (!_wfopen_s(&f, (root + L"\\xstation-wifi.log").c_str(), L"a") && f) { fputs(line.c_str(), f); fclose(f); }
}

void DeleteTree(const std::wstring& path, int depth = 0) {
    if (depth > 32) return;
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
            const std::wstring child = path + L"\\" + f.cFileName;
            if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) DeleteTree(child, depth + 1);
            else DeleteFileW(child.c_str());
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    RemoveDirectoryW(path.c_str());
}

// The worker's side: one range from the browser (false when the check was stopped or the page went away).
bool ProbeAsk(Probe& p, int pkg, uint64_t offset, uint64_t length, std::vector<uint8_t>& out) {
    std::unique_lock<std::mutex> lock(p.lock);
    p.asking = true;
    p.answered = false;
    p.askPkg = pkg;
    p.askOffset = offset;
    p.askLength = length;
    p.cv.notify_all();
    const bool got = p.cv.wait_for(lock, std::chrono::seconds(60), [&] { return p.answered || p.cancel; }) && p.answered;
    p.asking = false;
    if (!got || p.answer.size() != length) return false;
    out.swap(p.answer);
    p.answer.clear();
    p.answered = false;
    p.fetched += length;
    ++p.trips;
    return true;
}

// The PKG reader's reads, served from 256 KiB pieces asked for in runs: a run going on from where the last one ended
// doubles (up to 16 MiB), a jump starts again at 1 MiB, so the scattered tables cost little and a file's blocks come in a
// few big asks.
struct ProbeCache {
    static constexpr uint64_t kPiece = 256 << 10;
    std::map<uint64_t, std::vector<uint8_t>> pieces;
    uint64_t next = UINT64_MAX, ahead = 4;
};
constexpr uint64_t kProbeLimit = 768ull << 20; // past this much the check gives up (couldn't check)

bool ProbeRead(Probe& p, int pkg, ProbeCache& c, uint64_t offset, uint8_t* buf, size_t n) {
    const uint64_t size = p.sizes[(size_t)pkg];
#ifdef XSTATION_TRANSFER_LOOPBACK
    if (getenv("XS_PROBE_TRACE")) {
        char t[96];
        snprintf(t, sizeof(t), "  read %d %llu %zu (size %llu)\n", pkg, (unsigned long long)offset, n, (unsigned long long)size);
        WifiLog(t);
    }
#endif
    while (n) {
        const uint64_t index = offset / ProbeCache::kPiece;
        auto it = c.pieces.find(index);
        if (it == c.pieces.end()) {
            c.ahead = index == c.next ? std::min<uint64_t>(c.ahead * 2, 64) : 4;
            const uint64_t last = (size - 1) / ProbeCache::kPiece;
            uint64_t count = 0;
            while (count < c.ahead && index + count <= last && !c.pieces.count(index + count)) ++count;
            const uint64_t from = index * ProbeCache::kPiece, length = std::min(count * ProbeCache::kPiece, size - from);
            if (p.fetched + length > kProbeLimit) return false;
            std::vector<uint8_t> data;
            if (!ProbeAsk(p, pkg, from, length, data)) return false;
            for (uint64_t i = 0; i < count; ++i) {
                const uint64_t at = i * ProbeCache::kPiece, take = std::min(ProbeCache::kPiece, length - at);
                c.pieces[index + i].assign(data.begin() + (ptrdiff_t)at, data.begin() + (ptrdiff_t)(at + take));
            }
            c.next = index + count;
            it = c.pieces.find(index);
        }
        const uint64_t within = offset - index * ProbeCache::kPiece;
        if (within >= it->second.size()) return false;
        const size_t take = (size_t)std::min<uint64_t>(n, it->second.size() - within);
        memcpy(buf, it->second.data() + within, take);
        buf += take;
        offset += take;
        n -= take;
    }
    return true;
}

void ProbeWork(std::shared_ptr<Probe> p) {
    const auto began = std::chrono::steady_clock::now();
    const std::wstring tag = std::to_wstring(++probeCount);
    const std::wstring scratch = root + L"\\.wifi-check-" + tag;
    std::vector<std::wstring> paths;
    std::vector<std::shared_ptr<ProbeCache>> caches;
    for (size_t k = 0; k < p->sizes.size(); ++k) {
        paths.push_back(L"wifi-check-" + tag + L"\\" + (k ? L"update.pkg" : L"game.pkg"));
        auto cache = std::make_shared<ProbeCache>();
        caches.push_back(cache);
        PkgRemoteAdd(paths.back(), p->sizes[k], [p, k, cache](uint64_t offset, uint8_t* buf, size_t n) { return ProbeRead(*p, (int)k, *cache, offset, buf, n); });
    }
    DeleteTree(scratch);
    CreateDirectoryW(scratch.c_str(), nullptr);
    static const std::vector<std::string> only{"eboot.bin", "sce_module/", "sce_sys/param.sfo"};
    std::wstring error, engine;
    std::string detail;
    int result = -1;
    if (PkgExtractGame(paths, scratch, nullptr, nullptr, false, error, 0, nullptr, false, &only))
        result = GameMatchData(p->id, scratch, enginesRoot, engine, detail) ? 1 : 0;
    for (const auto& path : paths) PkgRemoteRemove(path);
    DeleteTree(scratch);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::unique_lock<std::mutex> lock(p->lock);
    if (p->cancel && result == -1) error = L"the check was stopped";
    char line[256];
    snprintf(line, sizeof(line), "wifi check %ls: %s%ls in %.1f s, %.1f MB read from the browser in %d asks\n", p->id.c_str(),
             result == 1 ? "compatible " : result == 0 ? "not compatible" : "couldn't check: ", result == 1 ? engine.c_str() : result == -1 ? error.c_str() : L"",
             seconds, p->fetched / 1048576.0, p->trips);
    WifiLog(line + detail);
    p->result = result;
    p->engine = engine;
    p->error = error;
    p->done = true;
    p->cv.notify_all();
}

// The server's side: waits for the worker's next ask or its result and replies with it.
void ProbeReply(SOCKET s, const std::shared_ptr<Probe>& p) {
    std::unique_lock<std::mutex> lock(p->lock);
    if (!p->cv.wait_for(lock, std::chrono::seconds(120), [&] { return p->done || (p->asking && !p->answered); })) {
        lock.unlock();
        Message(s, "500 Internal Server Error", L"The Xbox didn't finish checking the PKG. Send it again.");
        return;
    }
    if (!p->done) {
        char text[96];
        snprintf(text, sizeof(text), "need %d %llu %llu", p->askPkg, (unsigned long long)p->askOffset, (unsigned long long)p->askLength);
        lock.unlock();
        Reply(s, "200 OK", "text/plain", text, strlen(text));
        return;
    }
    const int result = p->result;
    const std::wstring id = p->id, engine = p->engine, error = p->error;
    lock.unlock();
    const bool known = GamePrepareSupported(GameEngineId(id));
    if (result == 1) {
        const std::string text = "ok " + std::string(engine.begin(), engine.end());
        Reply(s, "200 OK", "text/plain", text.data(), text.size());
    } else if (result == 0) {
        Message(s, "409 Conflict", known ? L"Not compatible: this copy's game files aren't the ones XStation plays (another version, or a modified copy), "
                                           L"so it wasn't sent."
                                         : L"Not compatible: XStation has no engine for this game (" + id + L"), and its game files aren't the same as "
                                           L"any game XStation plays, so it wasn't sent.");
    } else if (known) {
        Reply(s, "200 OK", "text/plain", "ok", 2); // couldn't read the files (a PKG type the check can't open): a known title still goes
    } else {
        Message(s, "409 Conflict", L"XStation couldn't check this PKG's game files (" + error + L"), and it has no engine for " + id +
                                   L", so it wasn't sent. Try it from a USB drive.");
    }
}

void ProbeStart(SOCKET s, const std::string& head, const std::string& target) {
    if (!Paired(s, head)) return;
    if (streaming) { Message(s, "409 Conflict", L"Another PKG is being converted. Send PKGs one at a time."); return; }
    const std::string id = Query(target, "id"), sizes = Query(target, "sizes");
    bool safe = id.size() == 9;
    for (const char c : id) safe = safe && isalnum((unsigned char)c);
    auto p = std::make_shared<Probe>();
    p->id.assign(id.begin(), id.end());
    for (size_t at = 0; at < sizes.size();) {
        size_t end = sizes.find(',', at);
        if (end == std::string::npos) end = sizes.size();
        p->sizes.push_back(_strtoui64(sizes.substr(at, end - at).c_str(), nullptr, 10));
        at = end + 1;
    }
    if (!safe || p->sizes.empty() || p->sizes.size() > 2) { Plain(s, "400 Bad Request"); return; }
    for (const uint64_t z : p->sizes) if (z < 0x1000) { Plain(s, "400 Bad Request"); return; }
    if (enginesRoot.empty()) { Reply(s, "200 OK", "text/plain", "ok", 2); return; }
    {
        std::lock_guard<std::mutex> lock(probeLock);
        if (currentProbe) {
            std::lock_guard<std::mutex> old(currentProbe->lock);
            currentProbe->cancel = true;
            currentProbe->cv.notify_all();
        }
        currentProbe = p;
    }
    std::thread(ProbeWork, p).detach();
    ProbeReply(s, p);
}

void ProbeData(SOCKET s, const std::string& head, const std::string& target, const char* extra, size_t extraSize) {
    if (!Paired(s, head)) return;
    std::shared_ptr<Probe> p;
    {
        std::lock_guard<std::mutex> lock(probeLock);
        p = currentProbe;
    }
    const uint64_t offset = _strtoui64(Query(target, "offset").c_str(), nullptr, 10);
    const uint64_t length = _strtoui64(Header(head, "content-length").c_str(), nullptr, 10);
    if (!p || length > (32ull << 20)) { Plain(s, "400 Bad Request"); return; }
    std::vector<uint8_t> data((size_t)length);
    size_t have = std::min<size_t>(extraSize, (size_t)length);
    memcpy(data.data(), extra, have);
    while (have < length) {
        const int n = recv(s, reinterpret_cast<char*>(data.data()) + have, (int)std::min<size_t>(length - have, 1 << 20), 0);
        if (n <= 0) { Plain(s, "400 Bad Request"); return; }
        have += (size_t)n;
    }
    {
        std::lock_guard<std::mutex> lock(p->lock);
        if (p->done || !p->asking || p->answered || offset != p->askOffset || length != p->askLength) {
            // not what was asked: the check goes on waiting for the right bytes
        } else {
            p->answer.swap(data);
            p->answered = true;
            p->cv.notify_all();
        }
    }
    ProbeReply(s, p);
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
    else if (method == "POST" && target.rfind("/probe/start?", 0) == 0) ProbeStart(s, head, target);
    else if (method == "PUT" && target.rfind("/probe/data?", 0) == 0) ProbeData(s, head, target, data.data() + end + 4, data.size() - end - 4);
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

void SetTransferEngines(const std::wstring& engines) { enginesRoot = engines; }

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

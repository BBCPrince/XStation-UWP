#pragma once
#include <Windows.h>
#include <bcrypt.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>
#include <array>
#include <string>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include "NativeSourceVariants.h"
#pragma comment(lib,"bcrypt.lib")
// Optional, own-process Xbox compatibility experiment. Checked compiler ABI;
// no public D3D12 cached-blob/library calls, no driver binaries distributed.
namespace SpiderNativeCompilerCache {
namespace fs=std::filesystem;
using Bytes=std::vector<unsigned char>;
using Compile=void*(__cdecl*)(void*,void*);
using Get=void*(__cdecl*)(void*);
using Serialize=void*(__cdecl*)(void*,void*);
using GetBytes=unsigned(__cdecl*)(void*,void**,uint64_t*);
using Deserialize=void*(__cdecl*)(void const*,uint64_t,void*);
using GetRestored=unsigned(__cdecl*)(void*,void**);
using Code=unsigned(__cdecl*)(void*);
using Destroy=void(__cdecl*)(void*);
inline Compile nativeCompile=nullptr;
inline Get nativeGet=nullptr;
inline Destroy nativeDestroy=nullptr;
inline Serialize serializeSource=nullptr,serializeOutput=nullptr;
inline GetBytes getSource=nullptr,getOutputBytes=nullptr;
inline Deserialize deserialize=nullptr;
inline GetRestored getRestored=nullptr;
inline Deserialize deserializeSource=nullptr;
inline GetRestored getRestoredSource=nullptr;
inline Code code=nullptr;
inline fs::path directory;
inline uint64_t driverIdentity=0;
inline bool initialized=false,installed=false;
inline bool(*hasRoom)(uint64_t)=nullptr;
inline std::recursive_mutex callMutex;
inline std::array<std::recursive_mutex,64> entryMutexes;
inline std::mutex storeMutex;
inline std::atomic<bool> startupBatchActive{false};
inline std::mutex resultMutex;
inline std::unordered_map<void*,std::shared_ptr<Bytes>> restoredResults;
inline thread_local bool active=false;
inline thread_local bool warmingSources=false,variantBusy=false;
inline unsigned variantMode=0;
inline std::mutex variantMutex;
inline std::unordered_set<std::wstring> variantSeen;
inline std::atomic<uint64_t> variantTried{0},variantReady{0},variantFailed{0},variantUnsupported{0};
inline void WarmSubsets(Bytes const& source,void* allocator);
inline std::atomic<uint64_t> evicted{0};
inline std::atomic<uint64_t> requests{0},hits{0},misses{0},saved{0},rejected{0},errors{0},nativeCalls{0},heldBytes{0},peakHeldBytes{0};
// Per-caller attribution only; no source data, allocations, or file I/O added.
struct Attribution {uint64_t calls=0,hits=0,misses=0,bypass=0,sourceMs=0,loadMs=0,restoreMs=0,compileMs=0,storeMs=0;};
inline thread_local Attribution* attribution=nullptr;
struct AttributionScope {
 Attribution data{};Attribution* previous;
 explicit AttributionScope(bool enabled):previous(attribution){attribution=enabled?&data:nullptr;}
 ~AttributionScope(){attribution=previous;}
 AttributionScope(const AttributionScope&)=delete;
};
struct AttributionTimer {
 uint64_t* target;uint64_t begin;
 explicit AttributionTimer(uint64_t* value):target(value),begin(value?GetTickCount64():0){}
 ~AttributionTimer(){if(target)*target+=GetTickCount64()-begin;}
};
constexpr uint64_t InputLimit=2*1024*1024,OutputLimit=4*1024*1024,DiskLimit=256ull*1024*1024;
constexpr uint64_t WorkingReserve=8*1024*1024;
inline uint64_t hash(void const* p,size_t n){uint64_t h=1469598103934665603ull;auto b=static_cast<unsigned char const*>(p);while(n--){h^=*b++;h*=1099511628211ull;}return h;}
inline bool Room(uint64_t bytes){return !hasRoom||hasRoom(bytes);}
inline std::array<unsigned char,32> digest(Bytes const& b){std::array<unsigned char,32> out{};if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,const_cast<unsigned char*>(b.data()),static_cast<ULONG>(b.size()),out.data(),32)<0)throw std::runtime_error("input digest");return out;}
inline std::wstring name(std::array<unsigned char,32> const& d){const wchar_t* hex=L"0123456789abcdef";std::wstring s;for(auto b:d){s+=hex[b>>4];s+=hex[b&15];}return s+L".bin";}
struct Header {uint64_t magic=0x3158434250434e53ull;uint32_t version=1,bytes=sizeof(Header);uint64_t driver=0,inputBytes=0,outputBytes=0,inputHash=0,outputHash=0;std::array<unsigned char,32> key{};};
inline Bytes read(fs::path const& p,size_t max){std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)return {};auto n=f.tellg();if(n<=0||static_cast<uint64_t>(n)>max)return {};Bytes b(static_cast<size_t>(n));f.seekg(0);if(!f.read(reinterpret_cast<char*>(b.data()),b.size()))return {};return b;}
inline Bytes SerializeBytes(Serialize operation,GetBytes get,void* input,void* allocator,uint64_t limit){
 auto r=operation(input,allocator);if(!r)return {};Bytes b;
 try{void* p=nullptr;uint64_t n=0;if(code(r)==0&&get(r,&p,&n)==0&&p&&n>=16&&n<=limit){auto q=static_cast<unsigned char const*>(p);b.assign(q,q+static_cast<size_t>(n));}}
 catch(...){nativeDestroy(r);throw;}nativeDestroy(r);return b;
}
inline std::shared_ptr<Bytes> load(Bytes const& input,std::array<unsigned char,32> const& key){
 auto path=directory/name(key);std::error_code fileError;auto bytes=fs::file_size(path,fileError);if(fileError||!bytes||bytes>sizeof(Header)+InputLimit+OutputLimit)return {};
 if(!Room(WorkingReserve+bytes*2)){++rejected;return {};}
 auto record=std::make_shared<Bytes>(read(path,sizeof(Header)+InputLimit+OutputLimit));if(record->empty())return {};
 if(record->size()<sizeof(Header)){++rejected;return {};}
 Header h{};memcpy(&h,record->data(),sizeof(h));
 if(h.magic!=Header{}.magic||h.version!=1||h.bytes!=sizeof(h)||h.driver!=driverIdentity||h.key!=key||h.inputBytes!=input.size()||h.outputBytes<16||h.outputBytes>OutputLimit||h.inputBytes+ h.outputBytes!=record->size()-sizeof(h)||h.inputHash!=hash(input.data(),input.size())||memcmp(record->data()+sizeof(h),input.data(),input.size())||h.outputHash!=hash(record->data()+sizeof(h)+input.size(),static_cast<size_t>(h.outputBytes))){++rejected;return {};}
 if(!Room(WorkingReserve+record->size()*2)){++rejected;return {};}
 // XStation (2026-11-10): a hit marks the record as used, for the eviction in store().
 {std::error_code touchError;fs::last_write_time(path,fs::file_time_type::clock::now(),touchError);}
 return record;
}
inline uint64_t diskBytes=0;
inline void store(Bytes const& input,std::array<unsigned char,32> const& key,Bytes const& output){
 std::lock_guard<std::mutex> writeLock(storeMutex);
 ULARGE_INTEGER available{};if(!GetDiskFreeSpaceExW(directory.c_str(),&available,nullptr,nullptr)||available.QuadPart<64ull*1024*1024+sizeof(Header)+input.size()+output.size())return;
 const auto size=sizeof(Header)+input.size()+output.size();auto path=directory/name(key);std::error_code oldError;const auto old=fs::file_size(path,oldError);const uint64_t oldSize=oldError?0:old;auto base=diskBytes>oldSize?diskBytes-oldSize:0;
 // XStation (2026-11-10): a full cache used to refuse every new record, so what a game needed later was
 // compiled again at every start (Detroit: ~200 pipelines, ~1 min, once its 128 MiB were full of records
 // from older builds). Now the records used longest ago go, down to three quarters of the limit.
 if(base+size>DiskLimit){
  std::vector<std::pair<fs::file_time_type,fs::path>> records;std::error_code listError;
  for(auto const& entry:fs::directory_iterator(directory,listError)){std::error_code e;if(entry.is_regular_file(e)&&entry.path().extension()==L".bin"&&entry.path()!=path){auto t=entry.last_write_time(e);if(!e)records.emplace_back(t,entry.path());}}
  std::sort(records.begin(),records.end(),[](auto const& a,auto const& b){return a.first<b.first;});
  uint64_t total=0;for(auto const& r:records){std::error_code e;auto n=fs::file_size(r.second,e);if(!e)total+=n;}
  for(auto const& r:records){if(total+size<=DiskLimit*3/4)break;std::error_code e;auto n=fs::file_size(r.second,e);if(!e&&fs::remove(r.second,e)&&!e){total-=(std::min)(total,static_cast<uint64_t>(n));++evicted;}}
  base=total;if(base+size>DiskLimit)return;
 }
 Header h;h.driver=driverIdentity;h.key=key;h.inputBytes=input.size();h.outputBytes=output.size();h.inputHash=hash(input.data(),input.size());h.outputHash=hash(output.data(),output.size());
 auto temporary=path;temporary+=L".tmp";
 {std::ofstream f(temporary,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<char const*>(&h),sizeof(h));f.write(reinterpret_cast<char const*>(input.data()),input.size());f.write(reinterpret_cast<char const*>(output.data()),output.size());f.flush();if(!f)throw std::runtime_error("cache write");}
 if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("cache commit");diskBytes=base+size;++saved;
}
inline void* __cdecl GetOutput(void* result){
 bool cached=false;{std::lock_guard<std::mutex> lock(resultMutex);cached=restoredResults.find(result)!=restoredResults.end();}
 if(cached){void* p=nullptr;if(getRestored(result,&p)==0)return p;return nullptr;}return nativeGet(result);
}
inline void __cdecl DestroyResult(void* result){
 std::shared_ptr<Bytes> held;
 {std::lock_guard<std::mutex> lock(resultMutex);auto it=restoredResults.find(result);if(it!=restoredResults.end()){held=std::move(it->second);restoredResults.erase(it);heldBytes-=held->size();}}
 // Keep serialized backing alive until the compiler finishes destroying the
 // restored result, even if the compiler points into its input byte stream.
 nativeDestroy(result);
}
inline void* __cdecl CompilePipeline(void* input,void* allocator){
 if(attribution)++attribution->calls;
 if(!active){if(attribution)++attribution->bypass;++nativeCalls;AttributionTimer timer(attribution?&attribution->compileMs:nullptr);return nativeCompile(input,allocator);}++requests;
 Bytes source;std::array<unsigned char,32> key{};std::unique_lock<std::recursive_mutex> entryLock;
 try{
  {AttributionTimer timer(attribution?&attribution->sourceMs:nullptr);if(Room(WorkingReserve+InputLimit*2))source=SerializeBytes(serializeSource,getSource,input,allocator,InputLimit);}
  if(!source.empty()){
   std::shared_ptr<Bytes> record;
   {AttributionTimer timer(attribution?&attribution->loadMs:nullptr);key=digest(source);entryLock=std::unique_lock<std::recursive_mutex>(entryMutexes[key[0]%entryMutexes.size()]);record=load(source,key);}
   if(record){AttributionTimer timer(attribution?&attribution->restoreMs:nullptr);Header h;memcpy(&h,record->data(),sizeof(h));auto bytes=record->data()+sizeof(h)+source.size();auto r=deserialize(bytes,h.outputBytes,allocator);
    if(r){void* output=nullptr;if(code(r)==0&&getRestored(r,&output)==0&&output){
      try{std::lock_guard<std::mutex> lock(resultMutex);auto added=restoredResults.emplace(r,record);if(!added.second)throw std::runtime_error("duplicate compiler result");heldBytes+=record->size();auto held=heldBytes.load();auto peak=peakHeldBytes.load();while(held>peak&&!peakHeldBytes.compare_exchange_weak(peak,held)){} }
      catch(...){nativeDestroy(r);throw;}
      ++hits;if(attribution)++attribution->hits;
      if(entryLock.owns_lock())entryLock.unlock();WarmSubsets(source,allocator);return r;
    }nativeDestroy(r);}++rejected;
   }
  }
 }catch(...){++errors;source.clear();}
 ++misses;++nativeCalls;if(attribution)++attribution->misses;void* result=nullptr;
 {AttributionTimer timer(attribution?&attribution->compileMs:nullptr);result=nativeCompile(input,allocator);}
 // A failed native compile is never persisted. Don't add export allocations
 // during pressure: compilation and its original result take precedence.
 try{AttributionTimer timer(attribution?&attribution->storeMs:nullptr);if(result&&!source.empty()&&code(result)==0&&Room(64*1024*1024)){auto output=nativeGet(result);if(output){auto bytes=SerializeBytes(serializeOutput,getOutputBytes,output,allocator,OutputLimit);if(!bytes.empty())store(source,key,bytes);}}}catch(...){++errors;}
 if(entryLock.owns_lock())entryLock.unlock();WarmSubsets(source,allocator);return result;
}
inline void WarmSubsets(Bytes const& source,void* allocator){
 if(!warmingSources||variantBusy||!variantMode||!deserializeSource||!getRestoredSource||source.empty())return;
 struct Busy{Busy(){variantBusy=true;}~Busy(){variantBusy=false;}} busy;
 try{NativeSourceVariants::Each(source,[&](Bytes next){
  if(!Room(64*1024*1024))return;
  if(variantMode==2&&variantTried.load()>=128)return;
  const auto key=name(digest(next));
  {std::lock_guard<std::mutex> guard(variantMutex);if(variantSeen.size()>=32768||!variantSeen.insert(key).second)return;}
  ++variantTried;void* restored=nullptr;void* compiled=nullptr;
  try{
   restored=deserializeSource(next.data(),next.size(),allocator);void* input=nullptr;
   if(!restored||code(restored)!=0||getRestoredSource(restored,&input)!=0||!input)throw std::runtime_error("source deserialize");
   // Require a canonical round trip before invoking the compiler. Never alias
   // two different source descriptions to a shared compiled cache record.
   auto check=SerializeBytes(serializeSource,getSource,input,allocator,InputLimit);
   if(check!=next)throw std::runtime_error("source roundtrip differs");
   compiled=CompilePipeline(input,allocator);
   if(!compiled||code(compiled)!=0||!GetOutput(compiled))throw std::runtime_error("source compile failed");
   ++variantReady;
  }catch(...){++variantFailed;}
  if(compiled)DestroyResult(compiled);if(restored)nativeDestroy(restored);
 });}catch(...){++variantUnsupported;}
}
struct WarmSourceScope{bool previous;explicit WarmSourceScope(bool enable=true):previous(warmingSources){warmingSources=enable;}~WarmSourceScope(){warmingSources=previous;}};
struct Hook {unsigned char* site=nullptr;Bytes saved;void* relay=nullptr;size_t patched=0;};
inline Hook compileHook,getHook,destroyHook;
inline void jump(unsigned char* p,void* address){p[0]=0xff;p[1]=0x25;memset(p+2,0,4);memcpy(p+6,&address,8);}
inline bool prepare(Hook& h,void* target,Bytes const& expected,size_t patched,unsigned kind){
 if(!target||memcmp(target,expected.data(),expected.size()))return false;h.saved=expected;h.patched=patched;h.site=static_cast<unsigned char*>(target);h.relay=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!h.relay)return false;auto p=static_cast<unsigned char*>(h.relay);size_t bytes=0;
 if(kind==0){memcpy(p,expected.data(),15);jump(p+15,h.site+15);bytes=29;}
 if(kind==1){memcpy(p,expected.data(),19);bytes=19;}
 if(kind==2){
  memcpy(p,expected.data(),9);p[8]=16; // Null-result branch skips the absolute call.
  p[9]=0xff;p[10]=0x15;p[11]=2;p[12]=p[13]=p[14]=0;p[15]=0xeb;p[16]=8;
  int32_t relative=0;memcpy(&relative,expected.data()+10,4);void* callee=h.site+14+relative;memcpy(p+17,&callee,8);memcpy(p+25,expected.data()+14,5);bytes=30;
 }
 DWORD prior=0;if(!VirtualProtect(h.relay,4096,PAGE_EXECUTE_READ,&prior)||!FlushInstructionCache(GetCurrentProcess(),h.relay,bytes))return false;return true;
}
inline bool patch(Hook& h,void* target){DWORD prior=0;if(!VirtualProtect(h.site,h.patched,PAGE_EXECUTE_READWRITE,&prior))return false;Bytes replacement(h.patched,0x90);jump(replacement.data(),target);memcpy(h.site,replacement.data(),replacement.size());DWORD ignored=0;if(!VirtualProtect(h.site,h.patched,prior,&ignored)||!FlushInstructionCache(GetCurrentProcess(),h.site,h.patched))RaiseFailFastException(nullptr,nullptr,0);return true;}
inline void undo(Hook& h,bool changed){if(h.site&&changed){DWORD prior=0,ignored=0;if(!VirtualProtect(h.site,h.patched,PAGE_EXECUTE_READWRITE,&prior))RaiseFailFastException(nullptr,nullptr,0);memcpy(h.site,h.saved.data(),h.patched);if(!VirtualProtect(h.site,h.patched,prior,&ignored)||!FlushInstructionCache(GetCurrentProcess(),h.site,h.patched))RaiseFailFastException(nullptr,nullptr,0);}if(h.relay)VirtualFree(h.relay,0,MEM_RELEASE);h=Hook{};}
inline bool Install(fs::path const& folder,uint64_t driver,bool(*room)(uint64_t)=nullptr){
 std::lock_guard<std::recursive_mutex> lock(callMutex);if(installed)return true;if(initialized)return false;
 HMODULE module=GetModuleHandleW(L"xbsc_xs.dll");if(!module)return false;initialized=true;
 auto base=reinterpret_cast<unsigned char*>(module);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);if(nt->FileHeader.TimeDateStamp!=1635409256||nt->OptionalHeader.SizeOfImage!=3940352)return false;
 auto address=[&](char const* name,uintptr_t rva)->void*{auto p=GetProcAddress(module,name);return p&&reinterpret_cast<uintptr_t>(p)-reinterpret_cast<uintptr_t>(module)==rva?reinterpret_cast<void*>(p):nullptr;};
 auto compile=address("XBSC_PipelineCompile",0x9fe4),getter=address("XBSC_GetPipelineCompileOutput",0xa05c),destroy=address("XBSC_DestroyResult",0xa7f4);
 serializeSource=reinterpret_cast<Serialize>(address("XBSC_SerializeSrcPipeline",0xa394));getSource=reinterpret_cast<GetBytes>(address("XBSC_GetSerializedSrcPipeline",0xa40c));serializeOutput=reinterpret_cast<Serialize>(address("XBSC_SerializePipelineCompileOutput",0xa504));getOutputBytes=reinterpret_cast<GetBytes>(address("XBSC_GetSerializedPipelineCompileOutput",0xa57c));deserialize=reinterpret_cast<Deserialize>(address("XBSC_DeserializePipelineCompileOutput",0xa5bc));getRestored=reinterpret_cast<GetRestored>(address("XBSC_GetDeserializedPipelineCompileOutput",0xa644));code=reinterpret_cast<Code>(address("XBSC_GetResultCode",0xa810));if(!serializeSource||!getSource||!serializeOutput||!getOutputBytes||!deserialize||!getRestored||!code)return false;
 deserializeSource=reinterpret_cast<Deserialize>(address("XBSC_DeserializeSrcPipeline",0xa44c));getRestoredSource=reinterpret_cast<GetRestored>(address("XBSC_GetDeserializedSrcPipeline",0xa4d4));
 bool compileChanged=false,getChanged=false,destroyChanged=false;
 try{
  directory=folder/(L"native-pipeline-cache-v1-"+std::to_wstring(driver)+L"-1635409256");fs::create_directories(directory);diskBytes=0;for(auto const& entry:fs::directory_iterator(directory))if(entry.is_regular_file()&&entry.path().extension()==L".bin")diskBytes+=entry.file_size();driverIdentity=driver;hasRoom=room;
  if(!prepare(compileHook,compile,{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x83,0x64,0x24,0x40,0x00,0x48,0x8b,0xc2},15,0)||!prepare(getHook,getter,{0x48,0x85,0xc9,0x74,0x0b,0x83,0x79,0x18,0x06,0x75,0x05,0x48,0x8b,0x41,0x28,0xc3,0x33,0xc0,0xc3},16,1)||!prepare(destroyHook,destroy,{0x48,0x83,0xec,0x28,0x48,0x85,0xc9,0x74,0x05,0xe8,0x02,0xe3,0xff,0xff,0x48,0x83,0xc4,0x28,0xc3},14,2))throw std::runtime_error("compiler code guard");
  nativeCompile=reinterpret_cast<Compile>(compileHook.relay);nativeGet=reinterpret_cast<Get>(getHook.relay);nativeDestroy=reinterpret_cast<Destroy>(destroyHook.relay);
  if(!(destroyChanged=patch(destroyHook,reinterpret_cast<void*>(&DestroyResult)))||!(getChanged=patch(getHook,reinterpret_cast<void*>(&GetOutput)))||!(compileChanged=patch(compileHook,reinterpret_cast<void*>(&CompilePipeline))))throw std::runtime_error("compiler observer patch");installed=true;return true;
 }catch(...){undo(compileHook,compileChanged);undo(getHook,getChanged);undo(destroyHook,destroyChanged);nativeCompile=nullptr;nativeGet=nullptr;nativeDestroy=nullptr;return false;}
}
struct Scope {std::unique_lock<std::recursive_mutex> lock;bool previous;explicit Scope(bool use=true):lock(callMutex,std::defer_lock),previous(active){if(use){lock.lock();active=installed;}}~Scope(){active=previous;}Scope(Scope const&)=delete;};
// Only the startup coordinator owns this guard, until every worker has joined.
// Ordinary gameplay callers still use Scope and the unchanged serial gate.
struct StartupBatch {
 std::unique_lock<std::recursive_mutex> lock;
 StartupBatch():lock(callMutex){if(!installed||startupBatchActive.exchange(true))throw std::runtime_error("parallel cache startup state");}
 ~StartupBatch(){startupBatchActive.store(false);}
 StartupBatch(StartupBatch const&)=delete;
};
struct StartupWorkerScope {
 bool previous;
 StartupWorkerScope():previous(active){if(!installed||!startupBatchActive.load())throw std::runtime_error("parallel cache outside startup");active=true;}
 ~StartupWorkerScope(){active=previous;}
 StartupWorkerScope(StartupWorkerScope const&)=delete;
};
// Probe teardown only. Production leaves code installed until process exit.
inline void Uninstall(){std::lock_guard<std::recursive_mutex> lock(callMutex);if(!installed)return;{std::lock_guard<std::mutex> r(resultMutex);if(!restoredResults.empty())throw std::runtime_error("restored results still live");}undo(compileHook,true);undo(getHook,true);undo(destroyHook,true);installed=false;initialized=false;nativeCompile=nullptr;nativeGet=nullptr;nativeDestroy=nullptr;}
}

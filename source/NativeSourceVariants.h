#pragma once
#include <array>
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <stdexcept>

// Checked serializer layout of the guarded Xbox compiler version. This only
// derives source requests; it never substitutes a compiled result for a
// different source, and never changes a gameplay request.
namespace NativeSourceVariants {
using Bytes=std::vector<unsigned char>;
constexpr uint64_t Null=0xdeadbeefdeadbeefull;
constexpr size_t HeaderBytes=200,Limit=2*1024*1024;
constexpr std::array<size_t,6> Slots{{24,32,40,48,56,80}};
inline uint64_t Read(const Bytes& b,size_t p){if(p>b.size()||8>b.size()-p)throw std::runtime_error("source range");uint64_t x;memcpy(&x,b.data()+p,8);return x;}
inline void Write(Bytes& b,size_t p,uint64_t x){if(p>b.size()||8>b.size()-p)throw std::runtime_error("source range");memcpy(b.data()+p,&x,8);}
struct Stage {unsigned slot;size_t begin,end;};
struct Layout {std::vector<Stage> stages;unsigned mask=0;};
inline Layout Inspect(const Bytes& b){
 if(b.size()<HeaderBytes||b.size()>Limit||Read(b,0)!=0x000002014d535346ull||Read(b,8)!=b.size()-16||Read(b,16)!=6)throw std::runtime_error("unsupported source serializer");
 for(size_t p:{size_t(88),size_t(96),size_t(104)})if(Read(b,p)!=Null)throw std::runtime_error("unsupported auxiliary source");
 Layout out;
 for(unsigned i=0;i<Slots.size();++i){auto p=Read(b,Slots[i]);if(p==Null)continue;if(p<HeaderBytes||p>=b.size())throw std::runtime_error("source stage bounds");out.stages.push_back({i,size_t(p),0});out.mask|=1u<<i;}
 if(!out.mask||((out.mask&32)&&out.mask!=32))throw std::runtime_error("source stage kind");
 std::sort(out.stages.begin(),out.stages.end(),[](auto a,auto c){return a.begin<c.begin;});
 if(out.stages.front().begin!=HeaderBytes)throw std::runtime_error("source prefix");
 for(size_t i=0;i<out.stages.size();++i){
  auto& s=out.stages[i];s.end=i+1<out.stages.size()?out.stages[i+1].begin:b.size();
  if(s.end<=s.begin||s.end-s.begin<112)throw std::runtime_error("source stage size");
  for(size_t p:{size_t(0),size_t(8)}){auto x=Read(b,s.begin+p);if(x!=0&&x!=Null)throw std::runtime_error("named shader source unsupported");}
  if(Read(b,s.begin+16)!=s.begin+96)throw std::runtime_error("source shader buffer");
  auto bindings=Read(b,s.begin+24);if(bindings<s.begin||bindings>=s.end)throw std::runtime_error("source bindings");
 }
 return out;
}
inline Bytes Select(const Bytes& b,unsigned mask,bool nullNames){
 auto l=Inspect(b);if(!mask||(mask&~l.mask))throw std::runtime_error("source subset mask");
 Bytes out(b.begin(),b.begin()+HeaderBytes);for(auto p:Slots)Write(out,p,Null);
 for(auto s:l.stages){
  if(!(mask&(1u<<s.slot)))continue;
  const auto start=out.size();out.insert(out.end(),b.begin()+s.begin,b.begin()+s.end);
  for(size_t p:{size_t(16),size_t(24)})Write(out,start+p,Read(b,s.begin+p)-s.begin+start);
  if(nullNames){Write(out,start,Null);Write(out,start+8,Null);}
  Write(out,Slots[s.slot],start);
 }
 Write(out,8,out.size()-16);Inspect(out);return out;
}
template<class F>void Each(const Bytes& b,F visit){
 const auto l=Inspect(b);if(l.mask&32)return;
 for(unsigned mask=l.mask;mask;mask=(mask-1)&l.mask)for(bool names:{false,true}){
  auto next=Select(b,mask,names);if(next!=b)visit(std::move(next));
 }
}
}

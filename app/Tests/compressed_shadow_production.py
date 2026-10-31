#!/usr/bin/env python3
"""Compile CPU-only blocks extracted from MRRenderer.mm; fail on source drift."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "app/Source/MetalRenderer/MRRenderer.mm").read_text()
HEADER = ROOT / "app/Source/MetalRenderer/MRCopyOnWriteBytes.h"

def block_at(brace):
	depth = 0
	for i in range(brace, len(SOURCE)):
		if SOURCE[i] == "{": depth += 1
		elif SOURCE[i] == "}":
			depth -= 1
			if depth == 0: return SOURCE[brace:i + 1]
	raise RuntimeError("unbalanced production C++ block")

def function(name):
	m = re.search(r"static\s+[\w:<>]+\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", SOURCE)
	if not m: raise RuntimeError(f"production function missing: {name}")
	b = SOURCE.find("{", m.start(), m.end())
	return SOURCE[m.start():b] + block_at(b)

def anchored(anchor, occurrence=0):
	pos = 0
	for _ in range(occurrence + 1):
		start = SOURCE.find(anchor, pos)
		if start < 0: raise RuntimeError(f"production extraction anchor missing: {anchor!r}")
		pos = start + len(anchor)
	brace = start + anchor.rfind("{") if "{" in anchor else SOURCE.find("{", start + len(anchor))
	if brace < 0: raise RuntimeError(f"opening brace missing after {anchor!r}")
	return block_at(brace)

MIP = function("mipExtent")
BCBYTES = function("bcBlockBytes")
STORE = function("storeCompressed")
REGION = anchored("if (s->bcKind && d->bcKind && !s->compressedShadow.empty())")
RESOURCE = anchored("if (s->bcKind && d->bcKind && !s->compressedShadow.empty())", 1)
READBACK = anchored("if (s->bcKind) {\t\t// the original blocks (the GPU holds a decoded copy)")

HARNESS = r'''#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "MRCopyOnWriteBytes.h"
using std::uint32_t; using std::uint64_t; using std::size_t;
using mr::CopyOnWriteBytes;
struct Texture { uint32_t width=1,height=1,depthOrLayers=1,dim=2,mips=1; int bcKind=1;
 std::unordered_map<uint64_t,mr::CopyOnWriteBytes> compressedShadow; };
struct Stats { std::atomic<int64_t> shadowBytes{0}; std::atomic<uint64_t> bcCopies{0},bcReadbacks{0}; };
static Stats gTextureStats;
static void logOnce(const std::string&,const char*,...) {}
struct Engine { uint8_t *mem=nullptr; uint64_t memSize=0; }; static Engine *R=nullptr;
static const uint64_t kMaxShadowTexels=128*128;
static uint64_t key(uint32_t m,uint32_t l){return ((uint64_t)m<<32)|l;}
'''
HARNESS += MIP + "\n" + BCBYTES + "\n" + STORE + "\n"
HARNESS += r'''
static void region(Texture *s,Texture *d,uint32_t sMip,uint32_t sLayer,uint32_t dMip,uint32_t dLayer,
 uint32_t sx,uint32_t sy,uint32_t sz,uint32_t dx,uint32_t dy,uint32_t dz,uint32_t w,uint32_t h,uint32_t dep){
 uint32_t mw,mh,md;mipExtent(s,sMip,mw,mh,md);
'''+REGION+r'''
}
static void resource(Texture *s,Texture *d){
'''+RESOURCE+r'''
}
static void readback(Texture *s,uint32_t mip,uint32_t layer,uint32_t x0,uint32_t y0,uint32_t z0,
 uint32_t w,uint32_t h,uint32_t d,uint32_t rowPitch,uint32_t slicePitch,uint64_t addr){
 bool three=s->dim==3; auto done=[](){};uint32_t mw,mh,md;mipExtent(s,mip,mw,mh,md);
'''+READBACK+r'''
}
static std::vector<uint8_t> bytes(const mr::CopyOnWriteBytes &b){return b.empty()?std::vector<uint8_t>():std::vector<uint8_t>(b.data(),b.data()+b.size());}
static void eq(const mr::CopyOnWriteBytes &b,const std::vector<uint8_t>&v){assert(b.size()==v.size());if(!v.empty())assert(std::equal(v.begin(),v.end(),b.data()));}
static std::vector<uint8_t> make(uint32_t w,uint32_t h,uint8_t seed){uint32_t p=(w+3)/4*8,r=(h+3)/4;std::vector<uint8_t>v(p*r);for(size_t i=0;i<v.size();i++)v[i]=uint8_t(seed+i*17);return v;}
int main(){
 Texture s,d;s.width=d.width=64;s.height=d.height=64;s.mips=d.mips=4;s.depthOrLayers=d.depthOrLayers=3;
 std::vector<std::vector<uint8_t>> ref(12);
 for(uint32_t m=0;m<4;m++)for(uint32_t l=0;l<3;l++){uint32_t side=std::max(1u,64u>>m);ref[m*3+l]=make(side,side,uint8_t(m*31+l*7));auto &v=s.compressedShadow[key(m,l)];v.resize(ref[m*3+l].size());std::copy(ref[m*3+l].begin(),ref[m*3+l].end(),v.mutableData());}
 resource(&s,&d);
 for(uint32_t m=0;m<4;m++)for(uint32_t l=0;l<3;l++)assert(s.compressedShadow[key(m,l)].data()==d.compressedShadow[key(m,l)].data());
 uint8_t patch[8]={201,202,203,204,205,206,207,208};storeCompressed(&s,1,1,4,4,0,4,4,1,8,8,patch,sizeof patch);std::copy(patch,patch+8,ref[4].begin()+72);eq(s.compressedShadow[key(1,1)],ref[4]);eq(d.compressedShadow[key(1,1)],make(32,32,38));
 Texture a=s;auto old=bytes(a.compressedShadow[key(1,1)]);auto ar=old;region(&a,&a,1,1,1,1,0,0,0,4,4,0,4,4,1);std::copy(old.begin(),old.begin()+8,ar.begin()+72);eq(a.compressedShadow[key(1,1)],ar);
 // Force insertion/rehash while retaining a source snapshot from the same map.
 Texture rehash;rehash.width=64;rehash.height=64;rehash.mips=4;rehash.depthOrLayers=3;auto sourceOnly=make(32,32,55);
 auto &only=rehash.compressedShadow[key(1,1)];only.resize(sourceOnly.size());std::copy(sourceOnly.begin(),sourceOnly.end(),only.mutableData());
 rehash.compressedShadow.max_load_factor(0.5f);rehash.compressedShadow.rehash(2);
 region(&rehash,&rehash,1,1,3,2,0,0,0,4,4,0,4,4,1);
 auto inserted=rehash.compressedShadow.find(key(3,2));assert(inserted!=rehash.compressedShadow.end());std::vector<uint8_t> insertedRef(32,0);
 std::copy(sourceOnly.begin(),sourceOnly.begin()+8,insertedRef.begin()+24);eq(inserted->second,insertedRef);eq(rehash.compressedShadow[key(1,1)],sourceOnly);
 Texture t;t.width=64;t.height=64;t.mips=4;t.depthOrLayers=3;auto tr=make(16,16,99);auto &tb=t.compressedShadow[key(2,2)];tb.resize(tr.size());std::copy(tr.begin(),tr.end(),tb.mutableData());
 region(&s,&t,1,1,2,2,0,0,0,4,4,0,4,4,1);std::copy(ref[4].begin(),ref[4].begin()+8,tr.begin()+40);eq(tb,tr);eq(s.compressedShadow[key(1,1)],ref[4]);
 std::vector<uint8_t> host(3*512,0xee);Engine e{host.data(),host.size()};R=&e;
 for(uint32_t l=0;l<3;l++){readback(&s,1,l,0,0,0,32,32,1,64,512,l*512);
  for(uint32_t row=0;row<8;row++)assert(std::equal(ref[3+l].begin()+row*64,ref[3+l].begin()+(row+1)*64,host.begin()+l*512+row*64));}
 s.compressedShadow.clear();eq(d.compressedShadow[key(1,1)],make(32,32,38));puts("compressed_shadow_production: ok");return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="compressed-shadow-") as tmp:
	dirpath = Path(tmp)
	cpp = dirpath / "test.cpp"
	binary = dirpath / "test"
	cpp.write_text(HARNESS)
	compiler = os.environ.get("CXX") or shutil.which("c++")
	if not compiler: raise RuntimeError("C++ compiler required for production shadow tests")
	# Exercise both the portable-test dialect and the renderer's C++20 dialect.
	# Keep deprecation warnings fatal, including libc++'s C++17 diagnostics.
	for standard in ("c++17", "c++20"):
		cmd = [compiler, "-std=" + standard, "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(HEADER.parent), str(cpp), "-o", str(binary)]
		subprocess.run(cmd, check=True)
		subprocess.run([str(binary)], check=True)

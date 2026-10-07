// C++20 CPU regressions and an optional host lookup benchmark (no Vulkan/GPU).
#include "gfx/vulkan/word_cache.h"
#include "gfx/vulkan/cpu_snapshot.h"
#include "gfx/vulkan/cache_key.h"
#include "gx2/sparse_register_masks.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <stdexcept>
#include <unordered_map>

static size_t allocations = 0;
void* operator new(size_t bytes) {
  ++allocations;
  if (void* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

static void keys() {
  gfxvk::WordCache<uint64_t> cache;
  std::vector<std::vector<uint64_t>> keys;
  std::mt19937_64 random(713);
  for (size_t i=0; i<16000; ++i) {
    std::vector<uint64_t> key(1 + i%106);
    for (auto& word : key) word = random();
    key[0]=i;
    cache.insert(key,i+1); keys.push_back(std::move(key));
  }
  const size_t before = allocations;
  for (size_t i=0; i<keys.size(); ++i) {
    require(cache.find(keys[i]) == i+1,"growth/probing lost a key");
    keys[i][0]^=UINT64_MAX;
    require(!cache.find(keys[i]),"unequal key hit");
    keys[i][0]^=UINT64_MAX;
  }
  require(before==allocations,"warm find allocated");
  cache.insert(keys[100],22222);
  require(cache.find(keys[100])==22222,"existing key did not update");
  std::array<uint64_t,2> prefix{17,123};
  cache.insert(std::span(prefix).first(1),17); cache.insert(prefix,18);
  require(cache.find(std::span(prefix).first(1))==17 && cache.find(prefix)==18,"prefix lengths alias");
  cache.insert({},19); require(cache.find({})==19,"empty key lost");
  cache.clear();
  for (const auto& key : keys) require(!cache.find(key),"clear retained a key");
  cache.insert(keys[0],91); require(cache.find(keys[0])==91,"reinsert after clear failed");
}
static void snapshots() {
  struct Slice { uint64_t buffer=0,size=0; const void* mapped=nullptr; bool cpuReadable=false; };
  gfxvk::CpuSnapshotCache<Slice,uint64_t,1> cache; // force identity collisions
  std::array<uint8_t,32> bytes{};
  uint64_t created=0;
  auto factory = [&](const void*,size_t size) {
    // A mapped address which must never be dereferenced by reuse checks.
    return Slice{++created,size,reinterpret_cast<void*>(uintptr_t(1)),false};
  };
  int a=0,b=0;
  auto first=cache.get(&a,1,1,bytes.data(),bytes.size(),factory);
  require(cache.get(&a,1,1,bytes.data(),bytes.size(),factory).buffer==first.buffer,"equal CPU bytes missed");
  bytes.back()=1;
  require(cache.get(&a,1,1,bytes.data(),bytes.size(),factory).buffer!=first.buffer,"changed final byte hit");
  require(cache.get(&a,1,1,bytes.data(),16,factory).size==16,"size change hit");
  auto next=cache.get(&a,1,1,bytes.data(),16,factory);
  require(cache.get(&b,1,1,bytes.data(),16,factory).buffer!=next.buffer,"colliding identity hit");
  next=cache.get(&a,1,1,bytes.data(),16,factory);
  require(cache.get(&a,2,1,bytes.data(),16,factory).buffer!=next.buffer,"device change hit");
  next=cache.get(&a,2,1,bytes.data(),16,factory);
  require(cache.get(&a,2,2,bytes.data(),16,factory).buffer!=next.buffer,"submission change hit");
  next=cache.get(&a,2,2,nullptr,0,factory);
  require(cache.get(&a,2,2,nullptr,0,factory).buffer==next.buffer,"empty payload missed");
  cache.reset();
  require(cache.get(&a,2,2,nullptr,0,factory).buffer!=next.buffer,"reset retained slice");
}
static void sparse() {
  struct Mask { uint32_t shader=~0u,pipeline=0; bool operator==(const Mask&) const = default; };
  auto classify=[](uint32_t reg) {
    return reg>=0xA000 && reg<0xA400 ? Mask{reg&7,reg} : Mask{};
  };
  gx2::SparseRegisterMasks<Mask,65536> table(Mask{},classify);
  for (uint32_t reg=0; reg<65536; ++reg) require(table[reg]==classify(reg),"sparse classification differs");
  require(table.bytes()<16384,"default pages were retained");
}
static void benchmark() {
  using Key=std::array<uint64_t,18>; // layout/count, two dynamic UBOs, four images
  std::array<Key,600> keys;
  gfxvk::WordCache<uint64_t> flat;
  std::unordered_map<std::string,uint64_t,gfxvk::CacheKeyHash,std::equal_to<>> old;
  for (size_t i=0; i<keys.size(); ++i) {
    for (size_t j=0; j<keys[i].size(); ++j) keys[i][j]=i*131+j*37;
    flat.insert(keys[i],i+1);
    old.emplace(std::string(reinterpret_cast<const char*>(keys[i].data()),sizeof(Key)),i+1);
  }
  constexpr size_t iterations=1000000;
  auto run=[&](bool useFlat) {
    uint64_t sum=0;
    const auto begin=std::chrono::steady_clock::now();
    for (size_t i=0; i<iterations; ++i) {
      const auto& key=keys[(i*17)%keys.size()];
      sum+=useFlat ? flat.find(key) : old.find(std::string_view(reinterpret_cast<const char*>(key.data()),sizeof(Key)))->second;
    }
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
    std::printf("%s: %.1f ns/find (checksum %llu)\n",useFlat?"flat words":"string unordered_map",double(ns)/iterations,(unsigned long long)sum);
  };
  run(false); run(true);
}
int main(int argc,char**) {
  try { keys(); snapshots(); sparse(); if (argc>1) benchmark(); }
  catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
  std::puts("Vulkan preparation CPU regressions passed");
}

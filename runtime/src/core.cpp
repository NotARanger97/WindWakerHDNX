// Guest memory, RPX loading, function dispatch, logging and HLE registry.
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/ldsyms.h>
#endif
#include "platform/host.h"
#include "write_watch.h"
#include <zlib.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "recomp_table.h"
#include "runtime.h"

bool g_trace_hle = false;
namespace config {
std::string game_dir = "game";
std::string save_dir = "save";
}  // namespace config

static std::mutex g_log_mutex;

// the last log lines, kept for crash logs (crash handlers read them without the lock)
static constexpr int kLogRing = 200, kLogLine = 240;
static char g_log_ring[kLogRing][kLogLine];
static std::atomic<uint32_t> g_log_next{0};

void log_msg(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    char* line = g_log_ring[g_log_next.load() % kLogRing];
    va_list ap2;
    va_copy(ap2, ap);
    vsnprintf(line, kLogLine, fmt, ap2);
    va_end(ap2);
    g_log_next++;
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "wwhd", fmt, ap);  // adb logcat -s wwhd
    va_end(ap);
#else
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
#endif
}

void log_ring_write(int fd, void (*out)(int, const char*, size_t)) {
    uint32_t n = g_log_next.load();
    uint32_t first = n > (uint32_t)kLogRing ? n - kLogRing : 0;
    for (uint32_t i = first; i < n; i++) {
        const char* l = g_log_ring[i % kLogRing];
        out(fd, l, strnlen(l, kLogLine));
        out(fd, "\n", 1);
    }
}

void fatal(const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        va_list ap;
        va_start(ap, fmt);
#ifdef __ANDROID__
        __android_log_vprint(ANDROID_LOG_FATAL, "wwhd", fmt, ap);
#else
        fprintf(stderr, "FATAL: ");
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
#endif
        va_end(ap);
    }
    abort();
}

// ---------------------------------------------------------------- memory
namespace mem {
static std::atomic<uint32_t> g_runtime_top{kRuntimeStart};

#ifdef __SWITCH__
uint8_t* switch_map_guest_memory();  // platform/switch/switch_host.cpp
extern "C" void switch_set_mem_base(uint8_t* base);
#endif
void init() {
#if defined(__SWITCH__)
    switch_set_mem_base(switch_map_guest_memory());
#elif defined(__APPLE__)
    mach_vm_address_t addr = (mach_vm_address_t)PPC_MEM_BASE;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, 0x100000000ull, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS) fatal("cannot reserve guest address space at %p (kr=%d)", PPC_MEM_BASE, kr);
    // null page guard: catches guest null-pointer accesses
    mprotect(PPC_MEM_BASE, 0x10000, PROT_NONE);
#elif defined(_WIN32)
    void* p = VirtualAlloc(PPC_MEM_BASE, 0x100000000ull, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if(p != PPC_MEM_BASE) fatal("cannot reserve guest address space (error=%lu)",GetLastError());
    DWORD old; if(!VirtualProtect(PPC_MEM_BASE,0x10000,PAGE_NOACCESS,&old)) fatal("cannot protect guest null page");
#else
    // Never replace existing mappings: requesting a hint and checking the result is safe on
    // systems whose headers lack MAP_FIXED_NOREPLACE.
#ifdef __ANDROID__
    // phones have little RAM and strict commit accounting: pages are committed on first touch
    void* p = mmap(PPC_MEM_BASE,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
#else
    void* p = mmap(PPC_MEM_BASE,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
#endif
    if(p != PPC_MEM_BASE) { if(p!=MAP_FAILED)munmap(p,0x100000000ull); fatal("cannot reserve guest address space at %p",PPC_MEM_BASE); }
    if(mprotect(PPC_MEM_BASE,0x10000,PROT_NONE))fatal("cannot protect guest null page");
#endif
    // texture change detection (write_watch.h): after the crash handler, which it chains to
    if (!wwatch::init(PPC_MEM_BASE, 0x100000000ull)) LOG("[mem] write tracking unavailable: textures use sampled change checks");
}

static std::mutex g_alloc_log_m;
static std::vector<AllocRec> g_alloc_log;

static uint32_t bump(std::atomic<uint32_t>& top, uint32_t size, uint32_t align, uint32_t end) {
    uint32_t cur = top.load();
    uint32_t start;
    do {
        start = (cur + align - 1) & ~(align - 1);
        if (start + size > end) fatal("runtime guest region exhausted");
    } while (!top.compare_exchange_weak(cur, start + size));
    memset(ptr(start), 0, size);
    return start;
}

__attribute__((noinline)) uint32_t runtime_alloc(uint32_t size, uint32_t align) {
    uint32_t a = bump(g_runtime_top, size, align, kHostStart);
    // who allocated (relative to the executable, stable across runs of one build): a loaded save
    // state requires the same guest-visible allocations at the same addresses
    uint64_t tag = (uint64_t)((uintptr_t)__builtin_return_address(0) - host::executable_base());
    std::lock_guard<std::mutex> lk(g_alloc_log_m);
    g_alloc_log.push_back({a, size, tag});
    return a;
}

static std::atomic<uint32_t> g_host_top{kHostStart};
uint32_t host_alloc(uint32_t size, uint32_t align) { return bump(g_host_top, size, align, kFixedStart); }

uint32_t runtime_top() { return g_runtime_top.load(); }
void raise_runtime_top(uint32_t top) {
    uint32_t cur = g_runtime_top.load();
    while (cur < top && !g_runtime_top.compare_exchange_weak(cur, top)) {}
}
std::vector<AllocRec> runtime_alloc_log() {
    std::vector<AllocRec> v;
    {
        std::lock_guard<std::mutex> lk(g_alloc_log_m);
        v = g_alloc_log;
    }
    std::sort(v.begin(), v.end(), [](const AllocRec& a, const AllocRec& b) { return a.addr < b.addr; });
    return v;
}

std::string read_cstr(uint32_t ea) {
    if (!ea) return {};
    return std::string((const char*)ptr(ea));
}

void write_cstr(uint32_t ea, const std::string& s, uint32_t max) {
    uint32_t n = (uint32_t)std::min<size_t>(s.size(), max - 1);
    memcpy(ptr(ea), s.data(), n);
    ptr(ea)[n] = 0;
}
}  // namespace mem

// ---------------------------------------------------------------- loader
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

#ifdef __SWITCH__
bool wua_read_file(const std::string& host, std::vector<uint8_t>& out);  // hle/fs.cpp
#endif
bool load_rpx(const std::string& path, LoadedModule& out) {
    std::vector<uint8_t> d;
#ifdef __SWITCH__
    if (!wua_read_file(path, d))
#endif
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        d.assign(std::istreambuf_iterator<char>(f), {});
    }
    uint32_t shoff = be32(&d[0x20]);
    uint16_t shentsize = be16(&d[0x2E]), shnum = be16(&d[0x30]);
    out.entry = be32(&d[0x18]);
    out.data_end = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t* sh = &d[shoff + i * shentsize];
        uint32_t type = be32(sh + 4), flags = be32(sh + 8), addr = be32(sh + 12), off = be32(sh + 16), size = be32(sh + 20);
        std::vector<uint8_t> data;
        if (type != 8 && size) {
            if (flags & 0x08000000) {
                uLongf dlen = be32(&d[off]);
                data.resize(dlen);
                if (uncompress(data.data(), &dlen, &d[off + 4], size - 4) != Z_OK) fatal("zlib error in section %d", i);
            } else {
                data.assign(d.begin() + off, d.begin() + off + size);
            }
        }
        if (type == 0x80000004 && data.size() >= 0x30) {  // RPL file info
            out.sda_base = be32(&data[0x24]);
            out.sda2_base = be32(&data[0x28]);
            out.stack_size = be32(&data[0x2C]);
        }
        // allocated sections in the text/data regions
        if ((flags & 2) && addr >= 0x02000000 && addr < 0xC0000000) {
            if (type == 8) {
                memset(mem::ptr(addr), 0, size);
                out.data_end = std::max(out.data_end, addr + size);
            } else {
                memcpy(mem::ptr(addr), data.data(), data.size());
                if (addr >= mem::kMem2Start) out.data_end = std::max(out.data_end, addr + (uint32_t)data.size());
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- dispatch
uint32_t g_ppc_dispatch_epoch = 1;
namespace dispatch {
static constexpr uint32_t kTextBase = 0x02000000;
static constexpr uint32_t kTextSize = 0x01000000;  // 16 MiB covers cking.rpx .text
static std::atomic<PpcFunc>* g_text_table;           // indexed by (addr - kTextBase) / 4
// lock-free direct tables for import slots and host functions (called through pointers a lot)
static constexpr uint32_t kSlotBase = 0xC0000000, kSlotSize = 0x40000;     // import stubs, 4-byte steps
static constexpr uint32_t kHostSize = 0x80000;                              // host functions, 8-byte steps
static std::atomic<PpcFunc> g_slot_table[kSlotSize / 4];
static std::atomic<PpcFunc> g_host_table[kHostSize / 8];
static std::unordered_map<uint32_t, PpcFunc> g_other;
static std::shared_mutex g_other_mutex;
static std::atomic<uint32_t> g_next_host{mem::kHleFuncBase};
static std::unordered_map<uint32_t, std::string> g_host_names;

static void invalidate_caches(PpcFunc previous, PpcFunc fn) {
    // A missing target cannot have been cached: resolution would have aborted.
    // In particular, registering a new host callback need not cool every site.
    if (previous && previous != fn)
        __atomic_add_fetch(&g_ppc_dispatch_epoch, 1u, __ATOMIC_RELEASE);
}

void init() {
    g_text_table = new std::atomic<PpcFunc>[kTextSize / 4]{};
    __atomic_add_fetch(&g_ppc_dispatch_epoch, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < g_recomp_func_count; i++) set(g_recomp_funcs[i].addr, g_recomp_funcs[i].fn);
    // imported functions are reachable through their import slot addresses
    for (unsigned i = 0; i < g_recomp_import_count; i++)
        if (g_recomp_imports[i].is_func) set(g_recomp_imports[i].slot, g_recomp_imports[i].fn);
}

void set(uint32_t addr, PpcFunc fn) {
    if (addr - kTextBase < kTextSize) {
        PpcFunc previous = g_text_table[(addr - kTextBase) >> 2].exchange(fn, std::memory_order_relaxed);
        invalidate_caches(previous, fn);
        return;
    }
    if (addr - kSlotBase < kSlotSize && !(addr & 3)) {
        PpcFunc previous = g_slot_table[(addr - kSlotBase) >> 2].exchange(fn, std::memory_order_relaxed);
        invalidate_caches(previous, fn);
        return;
    }
    if (addr - mem::kHleFuncBase < kHostSize && !(addr & 7)) {
        PpcFunc previous = g_host_table[(addr - mem::kHleFuncBase) >> 3].exchange(fn, std::memory_order_relaxed);
        invalidate_caches(previous, fn);
        return;
    }
    std::unique_lock lk(g_other_mutex);
    PpcFunc previous = g_other[addr];
    g_other[addr] = fn;
    invalidate_caches(previous, fn);
}

PpcFunc lookup(uint32_t addr) {
    if (addr - kTextBase < kTextSize) return g_text_table[(addr - kTextBase) >> 2].load(std::memory_order_relaxed);
    if (addr - kSlotBase < kSlotSize && !(addr & 3)) return g_slot_table[(addr - kSlotBase) >> 2].load(std::memory_order_relaxed);
    if (addr - mem::kHleFuncBase < kHostSize && !(addr & 7))
        return g_host_table[(addr - mem::kHleFuncBase) >> 3].load(std::memory_order_relaxed);
    std::shared_lock lk(g_other_mutex);
    auto it = g_other.find(addr);
    return it == g_other.end() ? nullptr : it->second;
}

std::vector<std::pair<uint32_t, std::string>> host_functions() {
    std::shared_lock lk(g_other_mutex);
    std::vector<std::pair<uint32_t, std::string>> v(g_host_names.begin(), g_host_names.end());
    std::sort(v.begin(), v.end());
    return v;
}

uint32_t register_host(PpcFunc fn, const char* name) {
    uint32_t a = g_next_host.fetch_add(8);
    set(a, fn);
    std::unique_lock lk(g_other_mutex);
    g_host_names[a] = name;
    return a;
}
}  // namespace dispatch

extern "C" PpcFunc ppc_resolve(Cpu* c) {
    PpcFunc f = dispatch::lookup(c->pc);
    if (!f) fatal("indirect branch to unknown address %08X (lr=%08X ctr=%08X)", c->pc, c->lr, c->ctr);
    return f;
}

extern "C" PpcFunc ppc_resolve_cached(Cpu* c, PpcCallCache* cache, uint32_t epoch) {
    uint32_t target = c->pc;
    PpcFunc fn = ppc_resolve(c);
    cache->target = target;
    cache->epoch = epoch;
    cache->fn = fn;
    return fn;
}

extern "C" PpcCallCache* ppc_icache_alloc(void) {
    auto* caches = static_cast<PpcCallCache*>(calloc(std::max<uint32_t>(g_ppc_icache_sites, 1), sizeof(PpcCallCache)));
    if (!caches) fatal("no memory for %u indirect call caches", g_ppc_icache_sites);
    return caches;
}

extern "C" void ppc_dispatch(Cpu* c) {
    PpcFunc f = ppc_resolve(c);
    MUSTTAIL return f(c);
}

uint32_t guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t save_lr = c->lr, save_ctr = c->ctr, save_sp = c->r[1];
    // open a minimal frame so the callee's LR save slot doesn't clobber our caller's frame
    c->r[1] -= 0x40;
    st32(c->r[1], save_sp);
    int i = 3;
    for (uint32_t a : args) c->r[i++] = a;
    c->lr = 0;
    c->pc = fn;
    ppc_dispatch(c);
    c->r[1] = save_sp;
    c->lr = save_lr;
    c->ctr = save_ctr;
    return c->r[3];
}

extern "C" void ppc_unimplemented(Cpu* c, uint32_t addr, uint32_t insn) {
    fatal("unimplemented instruction %08X at %08X (lr=%08X)", insn, addr, c->lr);
}

extern "C" void ppc_trap(Cpu* c, uint32_t addr) {
    fatal("guest trap at %08X (lr=%08X r3=%08X)", addr, c->lr, c->r[3]);
}

// ---------------------------------------------------------------- HLE registry
static std::vector<HleReg*>& hle_registry() {
    static std::vector<HleReg*> r;
    return r;
}
HleReg::HleReg(const char* l, const char* n, PpcFunc f) : lib(l), name(n), fn(f) { hle_registry().push_back(this); }

PpcFunc hle_find(const char* lib, const char* name) {
    std::string l = lib;
    if (l.size() > 4 && l.substr(l.size() - 4) == ".rpl") l.resize(l.size() - 4);
    for (HleReg* r : hle_registry())
        if (l == r->lib && strcmp(name, r->name) == 0) return r->fn;
    return nullptr;
}

PpcFunc hle_find_any(const char* name) {
    for (HleReg* r : hle_registry())
        if (strcmp(name, r->name) == 0) return r->fn;
    return nullptr;
}

extern "C" void hle_unimplemented(Cpu* c, const char* lib, const char* name) {
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    {
        std::lock_guard<std::mutex> lk(m);
        int& n = seen[std::string(lib) + "." + name];
        if (n++ < 3)
            log_msg("[hle] unimplemented %s.%s(%08X, %08X, %08X, %08X) lr=%08X", lib, name, c->r[3], c->r[4], c->r[5],
                    c->r[6], c->lr);
    }
    c->r[3] = 0;
}

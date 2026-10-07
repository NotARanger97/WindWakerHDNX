// Crash log helpers (crash_addr.h): module + offset of host addresses, annotated host backtrace.
//
// What runs inside a crash handler, and the compromises:
// - Formatting: snprintf into stack buffers, written with the handler's own output function; no heap.
// - Windows: GetModuleHandleExW(FROM_ADDRESS | UNCHANGED_REFCOUNT) and GetModuleFileNameW look the
//   address up in the loader's module list, which can take the loader lock. A crash while another
//   thread holds it (a DLL being loaded or unloaded at that moment) could hang the report instead of
//   finishing it; the handler already used stdio and std::filesystem, so this adds no new kind of
//   risk. The backtrace walks the faulting CONTEXT with RtlLookupFunctionEntry / RtlVirtualUnwind
//   (x64; the unwind tables of each module, no dbghelp, no symbols) and only reads stack slots that
//   lie inside the thread's stack.
// - POSIX: dladdr is not on the async-signal-safe list (glibc and dyld take a loader lock); a crash
//   inside dlopen/dlclose could hang it. backtrace_symbols_fd, used here before, calls dladdr
//   internally the same way, so this is the same exposure as before. glibc's backtrace() loads
//   libgcc_s with malloc on first use: prime() makes that first use happen at start.
#include "crash_addr.h"

#include <cstdio>
#include <cstring>

#ifndef __SWITCH__  // the Switch build has its own crash handler (main.cpp, switch_host.cpp)
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#endif

namespace crash_addr {

#ifdef _WIN32
#define CRASH_ADDR_FMT "%016llX"  // the style of Windows' own crash reports
#else
#define CRASH_ADDR_FMT "0x%llx"
#endif

static const char* base_name(const char* p) {
    const char* b = p;
    for (const char* s = p; *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

int describe(char* buf, size_t cap, uintptr_t addr, char* path, size_t path_cap) {
    if (!cap) return 0;
    buf[0] = 0;
    if (!addr) return 0;
#ifdef _WIN32
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) || !m)
        return 0;
    wchar_t wide[1024];
    DWORD wn = GetModuleFileNameW(m, wide, 1024);
    char full[1024];
    int fn = wn ? WideCharToMultiByte(CP_UTF8, 0, wide, (int)wn, full, (int)sizeof full - 1, nullptr, nullptr) : 0;
    full[fn > 0 ? fn : 0] = 0;
    if (!full[0]) strcpy(full, "?");
    const uintptr_t base = (uintptr_t)m;  // an HMODULE is the module's load address
    int n = fit(snprintf(buf, cap, " in %s+0x%llX (base " CRASH_ADDR_FMT ")", base_name(full),
                           (unsigned long long)(addr - base), (unsigned long long)base), cap);
#else
    Dl_info di{};
    if (!dladdr((const void*)addr, &di) || !di.dli_fname) return 0;
    const char* full = di.dli_fname;
    const uintptr_t base = (uintptr_t)di.dli_fbase;
    int n = fit(snprintf(buf, cap, " in %s+0x%llx (base " CRASH_ADDR_FMT ")", base_name(full),
                           (unsigned long long)(addr - base), (unsigned long long)base), cap);
    // the nearest exported symbol (functions that are not exported get their predecessor's name)
    if (di.dli_sname && di.dli_saddr && addr >= (uintptr_t)di.dli_saddr)
        n += fit(snprintf(buf + n, cap - n, " [%s+0x%llx]", di.dli_sname,
                            (unsigned long long)(addr - (uintptr_t)di.dli_saddr)), cap - n);
#endif
    if (path && path_cap) snprintf(path, path_cap, "%s", full);
    return n;
}

static void frame_line(int fd, Out out, int i, uintptr_t pc) {
    char buf[512];
    int n = fit(snprintf(buf, sizeof buf, "  #%-2d " CRASH_ADDR_FMT, i, (unsigned long long)pc), sizeof buf);
    n += describe(buf + n, sizeof buf - n, pc);
    if ((size_t)n < sizeof buf - 1) buf[n++] = '\n';
    out(fd, buf, n);
}

#ifdef _WIN32
void host_backtrace(int fd, Out out, const void* context) {
    out(fd, "  host backtrace:\n", 18);
#if defined(_M_X64) || defined(__x86_64__)
    if (context) {
        CONTEXT c = *(const CONTEXT*)context;
        const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();  // the handler runs on the faulting thread
        const DWORD64 lo = (DWORD64)tib->StackLimit, hi = (DWORD64)tib->StackBase;
        for (int i = 0; i < 48 && c.Rip; i++) {
            frame_line(fd, out, i, (uintptr_t)c.Rip);
            const DWORD64 sp = c.Rsp;
            DWORD64 image = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
            if (f) {
                PVOID handler_data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &handler_data, &frame, nullptr);
            } else {
                // a leaf function, or code without unwind tables: the return address is on top
                if (c.Rsp < lo || c.Rsp + 8 > hi) break;
                c.Rip = *(const DWORD64*)c.Rsp;
                c.Rsp += 8;
            }
            if (c.Rsp < lo || c.Rsp > hi || c.Rsp < sp) break;
        }
        return;
    }
#endif
    // elsewhere: from here (the first frames are the handler and the exception dispatcher)
    void* frames[48];
    USHORT nf = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < nf; i++) frame_line(fd, out, i, (uintptr_t)frames[i]);
}

void prime() {}
#else
void host_backtrace(int fd, Out out, const void*) {
    out(fd, "  host backtrace:\n", 18);
    void* frames[64];
    int nf = backtrace(frames, 64);
    for (int i = 0; i < nf; i++) frame_line(fd, out, i, (uintptr_t)frames[i]);
}

uintptr_t context_pc(const void* ucontext) {
    if (!ucontext) return 0;
    const ucontext_t* uc = (const ucontext_t*)ucontext;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    return (uintptr_t)uc->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__) && defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__linux__) && defined(__arm__)
    return (uintptr_t)uc->uc_mcontext.arm_pc;
#elif defined(__linux__) && defined(__i386__)
    return (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
#else
    (void)uc;
    return 0;
#endif
}

void prime() {
    void* frames[4];
    backtrace(frames, 4);
    Dl_info di;
    dladdr((const void*)&prime, &di);
}
#endif

}  // namespace crash_addr
#endif  // __SWITCH__

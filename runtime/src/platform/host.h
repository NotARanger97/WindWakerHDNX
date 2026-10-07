#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__SWITCH__)
#include <pthread.h>
#include <unistd.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <mach-o/ldsyms.h>
#include <mach-o/dyld.h>
#include <climits>
#include <pthread/qos.h>
#elif !defined(_WIN32) && !defined(__SWITCH__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif
#ifdef __SWITCH__
extern "C" void _start();  // libnx: first instruction of the program image
#endif
namespace host {
#if defined(__APPLE__) && defined(WWHD_HAS_VULKAN)
// Render command batches can create Objective-C temporaries inside MoltenVK.
void with_autorelease_pool(void (*fn)());
void with_autorelease_pool(void (*fn)(void*), void* context);
#else
inline void with_autorelease_pool(void (*fn)()) { fn(); }
inline void with_autorelease_pool(void (*fn)(void*), void* context) { fn(context); }
#endif
// The native call is synchronous: captured references remain valid and no heap
// allocation or callable lifetime extension is needed. Exceptions propagate.
template<class Fn> void with_autorelease_pool(Fn&& fn) {
 auto call = [&] { fn(); };
 with_autorelease_pool([](void* context) {
  (*static_cast<decltype(call)*>(context))();
 }, &call);
}
inline thread_local std::string thread_label;
inline void set_thread_name(const char* name) {
 thread_label=name;
#ifdef __APPLE__
 pthread_setname_np(name);
#elif defined(_WIN32)
 using SetDescription=HRESULT(WINAPI*)(HANDLE,PCWSTR);
 auto f=(SetDescription)GetProcAddress(GetModuleHandleW(L"Kernel32.dll"),"SetThreadDescription");
 if(f) { std::wstring text; for(unsigned char c:thread_label)text.push_back(c); f(GetCurrentThread(),text.c_str()); }
#elif defined(__SWITCH__)
#else
 pthread_setname_np(pthread_self(),thread_label.substr(0,15).c_str());
#endif
}
// Game and render threads: keep them on fast cores and ahead of background work. macOS: QoS
// user-interactive (the default QoS let macOS park them on efficiency cores). Windows: above-normal
// priority and no power throttling (hybrid P/E-core CPUs otherwise move busy threads to E-cores).
// Linux: a small nice boost where the process may raise priority (needs CAP_SYS_NICE; otherwise a
// no-op). WWHD_NO_QOS=1 leaves the thread untouched on every platform.
inline void boost_thread_priority() {
 if(getenv("WWHD_NO_QOS")) return;
#ifdef __APPLE__
 pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
#elif defined(_WIN32)
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
 // SetThreadInformation(ThreadPowerThrottling) exists from Windows 10 1709; looked up at run time
 struct PowerThrottling { ULONG Version, ControlMask, StateMask; };
 using SetInfo=BOOL(WINAPI*)(HANDLE,int,LPVOID,DWORD);
 static const auto set_info=(SetInfo)GetProcAddress(GetModuleHandleW(L"Kernel32.dll"),"SetThreadInformation");
 if(set_info) {
  PowerThrottling state{1 /* THREAD_POWER_THROTTLING_CURRENT_VERSION */,1 /* EXECUTION_SPEED */,0 /* off */};
  set_info(GetCurrentThread(),3 /* ThreadPowerThrottling */,&state,sizeof state);
 }
#elif !defined(__SWITCH__)  // the Switch places its threads itself (switch_set_helper_thread)
 setpriority(PRIO_PROCESS,(id_t)syscall(SYS_gettid),-5);  // EPERM without CAP_SYS_NICE: ignored
#endif
}
inline void get_thread_name(char* out,size_t size) {
 if(!size)return;
#ifdef __APPLE__
 pthread_getname_np(pthread_self(),out,size);
#else
 snprintf(out,size,"%s",thread_label.empty()?"host":thread_label.c_str());
#endif
}
#ifdef __SWITCH__
extern "C" void switch_set_background_thread();
#endif
// Cache writers and other work nobody waits for: run only in time the game and render threads leave
// idle (Switch: lowest application priority; elsewhere the OS scheduler is left alone).
inline void background_thread() {
#ifdef __SWITCH__
 switch_set_background_thread();
#endif
}
inline uintptr_t executable_base() {
#ifdef __APPLE__
 return (uintptr_t)&_mh_execute_header;
#elif defined(_WIN32)
 return (uintptr_t)GetModuleHandleW(nullptr);
#elif defined(__SWITCH__)
 return (uintptr_t)&::_start;
#else
 static int anchor;
 Dl_info info{}; return dladdr(&anchor,&info)?(uintptr_t)info.dli_fbase:0;
#endif
}
inline std::string executable_path() {
#ifdef _WIN32
 char path[32768]; DWORD n=GetModuleFileNameA(nullptr,path,sizeof path);return std::string(path,n);
#elif defined(__SWITCH__)
 return "sdmc:/switch/wwhd/wwhd.nro";
#elif !defined(__APPLE__)
 char path[4096];ssize_t n=readlink("/proc/self/exe",path,sizeof path);return n>0?std::string(path,n):std::string();
#else
 return {};
#endif
}
inline size_t page_size() {
#ifdef _WIN32
 SYSTEM_INFO info;GetSystemInfo(&info);return info.dwPageSize;
#elif defined(__SWITCH__)
 return 0x1000;
#else
 return (size_t)getpagesize();
#endif
}
inline bool memory_touched(void* p,size_t bytes) {
#ifdef _WIN32
 // MEM_WRITE_WATCH records written pages even after paging them out. Residency alone
 // cannot distinguish untouched zero pages from modified pages in the swap file.
 size_t n=(bytes+page_size()-1)/page_size(); std::vector<void*> pages(n);
 ULONG_PTR count=n; DWORD granularity=0;
 if(GetWriteWatch(0,p,bytes,pages.data(),&count,&granularity)!=0)return true;
 return count!=0;
#elif defined(__APPLE__)
 std::vector<char> pages((bytes+page_size()-1)/page_size());
 if(mincore(p,bytes,pages.data()))return true;
 for(auto page:pages)if(page!=0)return true;
 return false;
#else
 // Linux mincore reports residency, not whether a swapped-out page contains data.
 // Scan the mapped region to preserve every byte; this is slower than macOS/Windows.
 (void)p;(void)bytes;return true;
#endif
}
inline bool replace_file(const std::string& from,const std::string& to) {
#ifdef _WIN32
 return MoveFileExA(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#elif defined(__SWITCH__)
 remove(to.c_str()); return rename(from.c_str(),to.c_str())==0;  // FAT: rename does not replace
#else
 return rename(from.c_str(),to.c_str())==0;
#endif
}
// Portable mode (release packages): a file "portable.txt" next to the executable keeps every
// per-user file (settings, controls, save states, shader caches) in "user" next to the executable's
// folder (<folder>/bin/wwhd -> <folder>/user) instead of the user's Library / AppData / .config.
// Without the marker (source builds) nothing changes.
inline std::string exe_dir() {
 static const std::string dir=[]{
  std::string p;
#if defined(__APPLE__)
  char buf[4096]; uint32_t n=sizeof buf;
  if(_NSGetExecutablePath(buf,&n)==0){ char real[PATH_MAX]; p=realpath(buf,real)?real:buf; }
#elif defined(_WIN32)
  char buf[MAX_PATH*4]; DWORD n=GetModuleFileNameA(nullptr,buf,sizeof buf); if(n>0&&n<sizeof buf) p.assign(buf,n);
#else
  char buf[4096]; ssize_t n=readlink("/proc/self/exe",buf,sizeof buf-1); if(n>0) p.assign(buf,(size_t)n);
#endif
  size_t s=p.find_last_of("/\\");
  return s==std::string::npos?std::string():p.substr(0,s);
 }();
 return dir;
}
inline const std::string& portable_user_dir() {
 static const std::string dir=[]{
  std::string e=exe_dir();
  if(e.empty()) return std::string();
  FILE* f=fopen((e+"/portable.txt").c_str(),"rb");
  if(!f) return std::string();
  fclose(f);
  size_t s=e.find_last_of("/\\");
  return (s==std::string::npos?e:e.substr(0,s))+"/user";
 }();
 return dir;
}
inline bool portable() { return !portable_user_dir().empty(); }
inline std::string config_dir() {
 if(portable()) return portable_user_dir();
#ifdef __APPLE__
 const char* home=getenv("HOME");return std::string(home?home:".")+"/Library/Application Support/WWHD";
#elif defined(_WIN32)
 const char* root=getenv("APPDATA");return std::string(root?root:".")+"/WWHD";
#elif defined(__SWITCH__)
 return "sdmc:/switch/wwhd/config";
#else
 if(const char* xdg=getenv("XDG_CONFIG_HOME"))return std::string(xdg)+"/wwhd";
 const char* home=getenv("HOME");return std::string(home?home:".")+"/.config/wwhd";
#endif
}
}

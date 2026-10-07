#!/usr/bin/env python3
"""Host exercise of the actual Switch recorder, with simulated libnx events/NVK.

Usage: test_vk_record_thread.py VULKAN_INCLUDE CXX
Does not build Switch code or validate libnx ABI/NVK/GPU behavior. Tests ownership,
FIFO, wrap/full/idle wakeups, host synchronization, disabled mode, and sticky errors.
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gen", ROOT / "tools/switch/gen_vk_switch.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

SWITCH = r"""
#pragma once
#include <cstdint>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
using Result = unsigned;
#define R_FAILED(r) ((r) != 0)
struct Event { std::mutex m; std::condition_variable cv; bool set=false; };
inline Result eventCreate(Event*, bool) { return 0; }
inline Result eventFire(Event* e) { std::lock_guard l(e->m); e->set=true; e->cv.notify_one(); return 0; }
inline Result eventWait(Event* e, uint64_t) { std::unique_lock l(e->m); e->cv.wait(l,[&]{return e->set;}); e->set=false; return 0; }
inline Result svcSleepThread(int64_t) { std::this_thread::yield(); return 0; }
inline uint64_t armGetSystemTick() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
inline uint64_t armTicksToNs(uint64_t n) { return n; }
"""

DRIVER = r"""
#include "platform/switch/vk_record_thread.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
extern "C" void switch_set_helper_thread(int core,int priority) { assert(core==0 && priority==0x2D); }
namespace test {
std::thread::id mainThread=std::this_thread::get_id();
std::atomic<bool> hold{true}, entered{false}, checkCopies{true};
std::atomic<VkResult> failure{VK_SUCCESS};
std::vector<std::string> order;
template<class Tuple> void inspect(const char*, const Tuple&) {}
void inspect(const char*, const std::tuple<VkCommandBuffer,const VkRenderingInfo*>& t) {
    auto& r=*std::get<1>(t);
    assert(r.colorAttachmentCount==1 && r.pColorAttachments[0].clearValue.color.float32[0]==0.25f);
    assert(r.pDepthAttachment->clearValue.depthStencil.depth==0.75f);
    assert(r.pStencilAttachment->clearValue.depthStencil.stencil==17);
}
void inspect(const char*, const std::tuple<VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,uint32_t,const VkDescriptorSet*,uint32_t,const uint32_t*>& t) {
    assert(std::get<5>(t)[0]==VkDescriptorSet(uintptr_t(123)) && std::get<7>(t)[0]==64);
}
void inspect(const char*, const std::tuple<VkQueue,uint32_t,const VkSubmitInfo*,VkFence>& t) {
    auto& s=std::get<2>(t)[0];
    assert(s.pCommandBuffers[0]==VkCommandBuffer(uintptr_t(456)));
    assert(s.pWaitSemaphores[0]==VkSemaphore(uintptr_t(12)));
    assert(s.pSignalSemaphores[0]==VkSemaphore(uintptr_t(13)));
    assert(s.pWaitDstStageMask[0]==VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}
void inspect(const char*, const std::tuple<VkQueue,uint32_t,const VkSubmitInfo2*,VkFence>& t) {
    auto& s=std::get<2>(t)[0];
    assert(s.pCommandBufferInfos[0].commandBuffer==VkCommandBuffer(uintptr_t(456)));
    assert(s.pWaitSemaphoreInfos[0].value==55 && s.pSignalSemaphoreInfos[0].value==77);
}
void inspect(const char*, const std::tuple<VkQueue,const VkPresentInfoKHR*>& t) {
    auto& p=*std::get<1>(t);
    assert(p.pSwapchains[0]==VkSwapchainKHR(uintptr_t(99)) && p.pImageIndices[0]==3);
    assert(p.pWaitSemaphores[0]==VkSemaphore(uintptr_t(13)));
}
void inspect(const char*, const std::tuple<VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,uint32_t,uint32_t,const void*>& t) {
    const auto* p=static_cast<const unsigned char*>(std::get<5>(t));
    for(uint32_t i=0;i<std::get<4>(t);++i) assert(p[i]==0x5a);
}
template<class Ret,class Tuple> Ret call(const char* name,const Tuple& t) {
    if(!vkrecord::enabled()) assert(std::this_thread::get_id()==mainThread);
    else assert(std::this_thread::get_id()!=mainThread);
    if(std::string(name)=="vkBeginCommandBuffer") {
        entered=true; while(hold.load()) std::this_thread::yield();
    }
    order.emplace_back(name);
    if(checkCopies.load()) inspect(name,t);
    if constexpr(std::is_same_v<Ret,VkResult>) return failure.load();
}
}
#define VK_RECORD_CALL(ret,name,params,args,packed) \
extern "C" VKAPI_ATTR ret VKAPI_CALL fake_##name params { return test::call<ret>(#name,std::make_tuple args); }
#include "platform/switch/vk_record_calls.inc"
#undef VK_RECORD_CALL
VKAPI_ATTR VkResult VKAPI_CALL fakeStatus(VkDevice,VkFence) {
    assert(std::this_thread::get_id()==test::mainThread);
    assert(!test::order.empty() && test::order.back()=="vkQueuePresentKHR"); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL fakeDestroy(VkDevice,VkBuffer,const VkAllocationCallbacks*) {
    assert(std::this_thread::get_id()==test::mainThread);
    assert(test::order.back()=="vkCmdDraw");
}
extern "C" PFN_vkVoidFunction nvk_loaderless_GetInstanceProcAddr(VkInstance,const char* name) {
#define VK_RECORD_CALL(ret,n,params,args,packed) if(std::string(name)==#n) return (PFN_vkVoidFunction)fake_##n;
#include "platform/switch/vk_record_calls.inc"
#undef VK_RECORD_CALL
    if(std::string(name)=="vkGetFenceStatus") return (PFN_vkVoidFunction)fakeStatus;
    if(std::string(name)=="vkDestroyBuffer") return (PFN_vkVoidFunction)fakeDestroy;
    return nullptr;
}
extern "C" PFN_vkVoidFunction nvk_loaderless_GetDeviceProcAddr(VkDevice,const char* name) { return nvk_loaderless_GetInstanceProcAddr({},name); }
int main(int argc,char**) {
    auto cmd=VkCommandBuffer(uintptr_t(456)); auto queue=VkQueue(uintptr_t(2));
    if(argc>1) {
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        VkBaseInStructure bad{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr};info.pNext=&bad;
        vkCmdBeginRendering(cmd,&info); return 2; // must abort rather than shallow copy
    }
    if(!vkrecord::enabled()) {
        test::hold=false;test::checkCopies=false; vkCmdDraw(cmd,3,1,0,0);
        assert(test::order.size()==1); std::puts("disabled direct path passed");std::_Exit(0);
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
    while(!test::entered.load()) std::this_thread::yield();
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},depth=color,stencil=color;
    color.clearValue.color.float32[0]=0.25f;depth.clearValue.depthStencil.depth=0.75f;stencil.clearValue.depthStencil.stencil=17;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};rendering.colorAttachmentCount=1;
    rendering.pColorAttachments=&color;rendering.pDepthAttachment=&depth;rendering.pStencilAttachment=&stencil;
    vkCmdBeginRendering(cmd,&rendering);color={};depth={};stencil={};rendering={};
    VkDescriptorSet set=VkDescriptorSet(uintptr_t(123));uint32_t offset=64;
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,{},0,1,&set,1,&offset);set={};offset=0;
    unsigned char constants[128];std::memset(constants,0x5a,sizeof(constants));
    vkCmdPushConstants(cmd,{},VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),constants);std::memset(constants,0,sizeof(constants));
    vkCmdEndRendering(cmd);assert(vkEndCommandBuffer(cmd)==VK_SUCCESS);
    VkSemaphore wait=VkSemaphore(uintptr_t(12)),signal=VkSemaphore(uintptr_t(13));
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
    submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&wait;submit.pWaitDstStageMask=&stage;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&signal;
    assert(vkQueueSubmit(queue,1,&submit,{})==VK_SUCCESS);submit={};wait={};stage=0;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};cb.commandBuffer=cmd;
    VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},s=w;w.value=55;s.value=77;
    VkSubmitInfo2 submit2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};submit2.commandBufferInfoCount=1;submit2.pCommandBufferInfos=&cb;
    submit2.waitSemaphoreInfoCount=1;submit2.pWaitSemaphoreInfos=&w;submit2.signalSemaphoreInfoCount=1;submit2.pSignalSemaphoreInfos=&s;
    assert(vkQueueSubmit2(queue,1,&submit2,{})==VK_SUCCESS);submit2={};cb={};w={};s={};
    VkSwapchainKHR swap=VkSwapchainKHR(uintptr_t(99));uint32_t index=3;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&signal;
    present.swapchainCount=1;present.pSwapchains=&swap;present.pImageIndices=&index;
    assert(vkQueuePresentKHR(queue,&present)==VK_SUCCESS);present={};swap={};index=0;signal={};
    cmd={};test::hold=false;
    assert(vkGetFenceStatus({},{})==VK_SUCCESS);
    const std::vector<std::string> expected={"vkBeginCommandBuffer","vkCmdBeginRendering","vkCmdBindDescriptorSets","vkCmdPushConstants","vkCmdEndRendering","vkEndCommandBuffer","vkQueueSubmit","vkQueueSubmit2","vkQueuePresentKHR"};
    assert(test::order==expected);
    test::checkCopies=false;
    // Pause replay while >8 MiB are produced, guaranteeing backpressure/padding.
    test::hold=true;test::entered=false;vkBeginCommandBuffer(cmd,&begin);
    while(!test::entered.load()) std::this_thread::yield();
    std::thread release([]{std::this_thread::sleep_for(std::chrono::milliseconds(100));test::hold=false;});
    std::vector<unsigned char> big(256*1024,0x5a);
    for(int i=0;i<100;++i) vkCmdPushConstants(cmd,{},VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(big.size()),big.data());
    release.join();assert(vkrecord::drain()==VK_SUCCESS);assert(test::order.size()==expected.size()+101);
    // Exercise the park/publish race and wrap the ring again with small calls.
    for(int round=0;round<3000;++round) {vkCmdDraw(cmd,3,1,0,0);assert(vkrecord::drain()==VK_SUCCESS);}
    for(int i=0;i<250000;++i) vkCmdDraw(cmd,3,1,0,0);
    vkDestroyBuffer({}, {}, nullptr); // generated void destruction wrapper drains
    assert(test::order.size()==expected.size()+101+3000+250000);
    assert(vkGetDeviceProcAddr({},"vkCmdDraw")==reinterpret_cast<PFN_vkVoidFunction>(vkCmdDraw));
    test::failure=VK_ERROR_DEVICE_LOST;
    const size_t beforeFailure=test::order.size();
    assert(vkEndCommandBuffer(cmd)==VK_SUCCESS); // deferred result
    vkCmdDraw(cmd,3,1,0,0);
    VkPresentInfoKHR failedPresent{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    assert(vkQueuePresentKHR(queue,&failedPresent)==VK_SUCCESS);
    assert(vkGetFenceStatus({},{})==VK_ERROR_DEVICE_LOST); // must not enter NVK
    assert(test::order.size()==beforeFailure+1); // following commands/present skipped
    assert(vkrecord::drain()==VK_ERROR_DEVICE_LOST); // remains sticky
    vkrecord::report(120);
    std::puts("FIFO, ownership, wrap/full/idle, drain, proc lookup, sticky error passed");
    std::fflush(stdout);std::_Exit(0); // process-lifetime helper has no host teardown
}
"""


def main(include, cxx):
    include = str(Path(include).resolve())
    env = dict(os.environ)
    env["PATH"] = str(Path(cxx).resolve().parent) + os.pathsep + env.get("PATH", "")
    env.pop("WWHD_VK_RECORD_THREAD", None)
    env["WWHD_VK_STATS"] = "1"
    with tempfile.TemporaryDirectory(prefix="vk-record-") as directory:
        work = Path(directory)
        (work / "platform").mkdir()
        (work / "switch.h").write_text(SWITCH)
        (work / "platform/host.h").write_text('#pragma once\nnamespace host { inline void set_thread_name(const char*) {} }\n')
        (work / "driver.cpp").write_text(DRIVER)
        gen.main(str(Path(include) / "vulkan/vulkan_core.h"), str(ROOT / "runtime/src"), str(work / "vk_switch.cpp"))
        flags = [cxx, "-std=c++20", "-D__SWITCH__", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-pthread",
                 "-I" + str(work), "-I" + str(ROOT / "runtime/src"), "-I" + include]
        exe = work / ("test.exe" if os.name == "nt" else "test")
        subprocess.run(flags + [str(ROOT / "runtime/src/platform/switch/vk_record_thread.cpp"),
                                str(work / "vk_switch.cpp"), str(work / "driver.cpp"), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, timeout=30, env=env)
        subprocess.run([str(exe)], check=True, timeout=10, env={**env, "WWHD_VK_RECORD_THREAD": "0"})
        refused = subprocess.run([str(exe), "unknown-pNext"], timeout=10, env=env, capture_output=True)
        assert refused.returncode != 0 and b"non-null pNext" in refused.stderr
        # A new runtime command must fail generation, even via a proc-name string.
        source = work / "src"
        (source / "platform/switch").mkdir(parents=True)
        (source / "platform/switch/vk_record_calls.inc").write_text((ROOT / "runtime/src/platform/switch/vk_record_calls.inc").read_text())
        (source / "new.cpp").write_text('auto name = "vkCmdDispatch";')
        try:
            gen.main(str(Path(include) / "vulkan/vulkan_core.h"), str(source), str(work / "invalid.cpp"))
        except ValueError as error:
            assert "vkCmdDispatch" in str(error)
        else:
            raise AssertionError("unlisted command silently forwarded")
        print("unknown command and pNext refusal passed")


if __name__ == "__main__":
    main(*sys.argv[1:3])

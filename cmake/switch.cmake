# Nintendo Switch (homebrew, libnx) build: Vulkan renderer on Mesa NVK, the SDL host on a libnx
# shim (runtime/src/platform/switch). Configure with devkitPro's toolchain:
#   cmake -S . -B build/switch -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
#         -DSWITCH_MESA_SDK_ROOT=<NVK SDK with libnvk_local.o> -DVULKAN_HEADERS=<Vulkan-Headers/include>
# devkitA64's GCC supports musttail (GCC 15+), so Clang is not required here.
include(FetchContent)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(SWITCH_MESA_SDK_ROOT "" CACHE PATH "Mesa NVK SDK (libnvk_local.o, include/)")
set(VULKAN_HEADERS "" CACHE PATH "Vulkan-Headers include directory")
set(PORTLIBS "${DEVKITPRO}/portlibs/switch")
add_compile_definitions(__SWITCH__)
set(CPU_FLAGS -mcpu=cortex-a57+crc+crypto)

# WWHD_SWITCH_SDK=ON: no game code and no .nro. Builds the runtime prelinked with libnx and the C/C++
# libraries into sdk/runtime.o, which the PC builder (tools/switch/builder.py) links with the game code
# it compiles from the player's own dump (clang and lld from the pinned zig: no devkitPro needed there).
option(WWHD_SWITCH_SDK "build the Switch SDK for the PC builder instead of the .nro" OFF)

# ---- recompiled game code
if(NOT WWHD_SWITCH_SDK)
  set(GEN_DIR ${CMAKE_SOURCE_DIR}/build/gen CACHE PATH "recompiler output")
  file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${GEN_DIR}/code_*.c)
  if(NOT GEN_SOURCES)
    message(FATAL_ERROR "no generated code in ${GEN_DIR}; run tools/recomp/recomp.py first")
  endif()
  add_library(gamecode STATIC ${GEN_SOURCES} ${GEN_DIR}/table.c ${GEN_DIR}/imports.c)
  target_include_directories(gamecode PUBLIC runtime/include ${GEN_DIR})
  set(GAMECODE_OPT "-O2" CACHE STRING "optimization level of the recompiled game code (-O2: game thread ~1-2% faster than -O3 on the hill replay, smaller code)")
  target_compile_options(gamecode PRIVATE ${GAMECODE_OPT} ${CPU_FLAGS} -ffp-contract=off -fno-strict-aliasing -w)
endif()

# ---- vendored Cemu GPU pieces: address library + shader decompiler (GLSL emitter)
set(CEMU_DIR ${CMAKE_SOURCE_DIR}/runtime/third_party/cemu)
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS ${CEMU_DIR}/*.cpp)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteDecompilerEmitMSL[^/]*\\.cpp$")
add_library(cemu_latte STATIC ${CEMU_SOURCES})
target_include_directories(cemu_latte PUBLIC ${CEMU_DIR} ${CEMU_DIR}/Cafe ${CMAKE_SOURCE_DIR}/runtime/third_party/metal-cpp
                                             ${CMAKE_SOURCE_DIR}/runtime/third_party/fmt/include ${VULKAN_HEADERS})
target_compile_options(cemu_latte PRIVATE "SHELL:-include ${CEMU_DIR}/cemu_shim.h" -w -O2 ${CPU_FLAGS})
target_compile_definitions(cemu_latte PUBLIC ENABLE_VULKAN=1)

# ---- glslang (same pinned version as Cemu-nx's Switch build)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
  GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
  GIT_TAG        1062752a891c95b2bfeed9e356562d88f9df84ac) # 15.1.0
FetchContent_MakeAvailable(glslang)
# the renderer includes <glslang/SPIRV/...>: expose the source tree under a "glslang" directory
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/glslang-inc)
file(CREATE_LINK ${glslang_SOURCE_DIR} ${CMAKE_BINARY_DIR}/glslang-inc/glslang SYMBOLIC)

# ---- Vulkan entry points forwarded to NVK (no loader on the Switch)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VK_SHIM ${CMAKE_BINARY_DIR}/vk_switch.cpp)
# Reconfigure the loaderless shim when its policy or runtime entry point use changes.
file(GLOB_RECURSE VK_SHIM_INPUTS CONFIGURE_DEPENDS runtime/src/*.cpp runtime/src/*.h runtime/src/*.c)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VK_SHIM_INPUTS}
  ${CMAKE_SOURCE_DIR}/tools/switch/gen_vk_switch.py
  ${CMAKE_SOURCE_DIR}/runtime/src/platform/switch/vk_record_calls.inc)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/switch/gen_vk_switch.py
                        ${VULKAN_HEADERS}/vulkan/vulkan_core.h ${CMAKE_SOURCE_DIR}/runtime/src ${VK_SHIM}
                RESULT_VARIABLE vk_shim_rc)
if(vk_shim_rc)
  message(FATAL_ERROR "gen_vk_switch.py failed")
endif()

# ---- ZArchive reader (Cemu's .wua format): the game is read straight from the archive
add_library(zarchive STATIC runtime/third_party/zarchive/zarchivereader.cpp runtime/third_party/zarchive/sha_256.c)
target_include_directories(zarchive PUBLIC runtime/third_party/zarchive/include ${PORTLIBS}/include)
target_compile_options(zarchive PRIVATE -O2 ${CPU_FLAGS} -w)

# ---- Dear ImGui (runtime/third_party/imgui): the in-game settings overlay, drawn by the Vulkan renderer
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/runtime/third_party/imgui)
add_library(imgui STATIC ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR})
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
target_compile_options(imgui PRIVATE -w -O2 ${CPU_FLAGS})

# ---- the game executable
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.cpp runtime/src/*.c runtime/src/hle/*.cpp runtime/src/mods/*.cpp
                                            runtime/src/overlay/*.cpp)
list(APPEND RUNTIME_SOURCES runtime/src/platform/perf_hint.cpp)  # Android performance hints (no-op here)
file(GLOB GX2_SOURCES CONFIGURE_DEPENDS runtime/src/gx2/*.cpp)
file(GLOB VULKAN_SOURCES CONFIGURE_DEPENDS runtime/src/gfx/vulkan/*.cpp)
set(GFX_SOURCES runtime/src/gfx/renderer.cpp runtime/src/gfx/display_modes.cpp ${VULKAN_SOURCES}
                runtime/src/platform/input_sdl.cpp runtime/src/platform/mouse_sdl.cpp)
set(SWITCH_SOURCES runtime/src/platform/switch/sdl_switch.cpp runtime/src/platform/switch/switch_host.cpp
                   runtime/src/platform/switch/switch_name.cpp
                   runtime/src/platform/switch/vk_record_thread.cpp
                   runtime/src/platform/switch/mem_base.c ${VK_SHIM})
# -fno-threadsafe-statics: per-draw paths test dozens of function-local static flags; the thread-safe
# guard check is a load-acquire each time (~3-4% of the render thread). Their statics are first used
# during single-threaded renderer setup or on the render thread itself.
set_source_files_properties(${GX2_SOURCES} ${GFX_SOURCES} PROPERTIES COMPILE_OPTIONS "-include;${CEMU_DIR}/cemu_shim.h;-fno-threadsafe-statics")
set_source_files_properties(runtime/src/platform/switch/vk_record_thread.cpp PROPERTIES COMPILE_OPTIONS "-fno-threadsafe-statics")
add_library(wwhd_runtime OBJECT ${RUNTIME_SOURCES} ${GX2_SOURCES} ${GFX_SOURCES} ${SWITCH_SOURCES})
target_include_directories(wwhd_runtime PRIVATE runtime/include runtime/src runtime/src/gx2 runtime/src/platform/switch
                                        ${VULKAN_HEADERS} ${SWITCH_MESA_SDK_ROOT}/include ${PORTLIBS}/include
                                        ${glslang_SOURCE_DIR} ${CMAKE_BINARY_DIR}/glslang-inc ${glslang_BINARY_DIR}/include)
target_compile_definitions(wwhd_runtime PRIVATE WWHD_HAS_VULKAN=1 WWHD_SDL_HOST=1)
target_compile_options(wwhd_runtime PRIVATE -O2 ${CPU_FLAGS} -fno-omit-frame-pointer -Wall -Wno-unused-function -Wno-unused-variable -ffp-contract=off)
add_dependencies(wwhd_runtime glslang)
set(RUNTIME_LIBS cemu_latte imgui zarchive glslang glslang-default-resource-limits)
target_link_libraries(wwhd_runtime PRIVATE ${RUNTIME_LIBS})  # their include directories and definitions
set(RUNTIME_LIB_FILES ${SWITCH_MESA_SDK_ROOT}/libnvk_local.o
  ${PORTLIBS}/lib/libelf.a ${PORTLIBS}/lib/libexpat.a ${PORTLIBS}/lib/libzstd.a
  ${PORTLIBS}/lib/libdrm_nouveau.a ${PORTLIBS}/lib/liblz4.a ${PORTLIBS}/lib/libz.a)
if(WWHD_SWITCH_SDK)
  # runtime.o: crti/crtbegin, the runtime, its libraries, libnx and newlib/libstdc++/libgcc in one relocatable
  # object (no debug info); crtend/crtn, the linker script and the headers the game code includes beside it
  get_filename_component(A64_BIN ${CMAKE_C_COMPILER} DIRECTORY)
  execute_process(COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name OUTPUT_VARIABLE LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
  get_filename_component(GCC_LIB_DIR ${LIBGCC} DIRECTORY)
  set(SDK_DIR ${CMAKE_BINARY_DIR}/sdk)
  set(SDK_LIBS)
  foreach(lib ${RUNTIME_LIBS})
    list(APPEND SDK_LIBS $<TARGET_FILE:${lib}>)
  endforeach()
  add_custom_command(OUTPUT ${SDK_DIR}/runtime.o
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/switch/sdk_prelink.py
            --ld ${A64_BIN}/aarch64-none-elf-ld --gcc-lib ${GCC_LIB_DIR} --devkitpro ${DEVKITPRO} --source ${CMAKE_SOURCE_DIR}
            --out ${SDK_DIR} --libs ${SDK_LIBS} ${RUNTIME_LIB_FILES} --objects $<TARGET_OBJECTS:wwhd_runtime>
    DEPENDS wwhd_runtime ${RUNTIME_LIBS} ${CMAKE_SOURCE_DIR}/tools/switch/sdk_prelink.py
    COMMAND_EXPAND_LISTS VERBATIM)
  add_custom_target(switch_sdk ALL DEPENDS ${SDK_DIR}/runtime.o)
else()
  add_executable(wwhd $<TARGET_OBJECTS:wwhd_runtime>)
  target_link_libraries(wwhd PRIVATE gamecode ${RUNTIME_LIBS} ${RUNTIME_LIB_FILES} nx m)
  nx_generate_nacp(wwhd.nacp NAME "Wind Waker HD" AUTHOR "ZeldaWWHDRecomp (Switch port)" VERSION "0.1")
  nx_create_nro(wwhd NACP wwhd.nacp)
endif()

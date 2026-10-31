cmake_minimum_required(VERSION 3.16)
project(infravr LANGUAGES C CXX)

# INFRA is a 32-bit game: build with -A Win32 (MSVC) or an i686 MinGW toolchain.
# The OpenXR helper (helper/openxr) is 64-bit and has its own CMake project.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "INFRA VR must be built as 32-bit (x86)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Shown in infravr.log. Releases are numbered by the maintainer: pass -DINFRAVR_VERSION=...
set(INFRAVR_VERSION "dev" CACHE STRING "Version string written to infravr.log")
option(INFRAVR_TESTS "Build the test programs" OFF)

add_library(minhook STATIC
    ${TP}/minhook/src/buffer.c ${TP}/minhook/src/hook.c ${TP}/minhook/src/trampoline.c
    ${TP}/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include)

set(INFRAVR_MODULE_DIRS src/core src/engine src/render src/vr src/gameplay src/ui)
set(INFRAVR_SOURCES
    src/core/log.cpp src/core/config.cpp src/core/flags.cpp
    src/engine/mathlib.cpp src/engine/hooks.cpp src/engine/game.cpp src/engine/hooks_game.cpp
    src/render/d3d9_proxy.cpp src/render/d3d_vr.cpp src/render/xr_publish.cpp src/render/world_size.cpp
    src/render/glb_model.cpp src/render/hands_renderer.cpp
    src/vr/openvr_rt.cpp src/vr/openxr_rt.cpp src/vr/overlay_api.cpp
    src/gameplay/mod.cpp
    src/ui/gamepad.cpp)

# MinGW's .def maps the plain export names to the decorated stdcall symbols; MSVC needs the plain
# names only (it resolves the decoration itself).
if(MSVC)
    set(INFRAVR_DEF src/render/d3d9_msvc.def)
else()
    set(INFRAVR_DEF src/render/d3d9.def)
endif()
add_library(infravr SHARED ${INFRAVR_SOURCES} ${INFRAVR_DEF})
set_target_properties(infravr PROPERTIES OUTPUT_NAME d3d9 PREFIX "")
target_include_directories(infravr PRIVATE ${INFRAVR_MODULE_DIRS} ${TP}/openvr ${TP}/vulkan ${TP}/cgltf ${TP}/stb)
target_compile_definitions(infravr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    INFRAVR_VERSION="${INFRAVR_VERSION}")
target_link_libraries(infravr PRIVATE minhook psapi user32)
if(MSVC)
    set_property(TARGET infravr minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(infravr PRIVATE /W3 /permissive-)
else()
    target_compile_options(infravr PRIVATE -O2 -Wall -Wno-unknown-pragmas -Wno-cast-function-type)
    target_link_options(infravr PRIVATE -s -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
    # Third-party single-header libraries: their warnings are not ours.
    set_source_files_properties(src/render/glb_model.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

if(INFRAVR_TESTS)
    # Each test is a console program for Windows (runs under Wine too).
    function(infravr_test name)
        add_executable(${name} ${ARGN})
        target_include_directories(${name} PRIVATE ${INFRAVR_MODULE_DIRS} ${TP}/openvr ${TP}/vulkan ${TP}/cgltf ${TP}/stb)
        target_compile_definitions(${name} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
            INFRAVR_VERSION="test")
        if(NOT MSVC)
            target_compile_options(${name} PRIVATE -O1 -w)
            target_link_options(${name} PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endfunction()
    infravr_test(glb_test tests/glb_test.cpp src/render/glb_model.cpp)
    infravr_test(resolve_test tests/resolve_test.cpp src/core/log.cpp src/core/config.cpp src/core/flags.cpp
        src/engine/hooks.cpp src/engine/game.cpp src/engine/mathlib.cpp src/render/world_size.cpp)
    target_link_libraries(resolve_test PRIVATE minhook psapi)
    infravr_test(dll_smoke tests/dll_smoke.cpp)
    infravr_test(shader_test tests/shader_test.cpp)
endif()

cmake_minimum_required(VERSION 3.16)
project(infravr LANGUAGES C CXX)

# INFRA is a 32-bit game: build with -A Win32 (MSVC) or an i686 MinGW toolchain.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "INFRA VR must be built as 32-bit (x86)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(minhook STATIC
    ${TP}/minhook/src/buffer.c ${TP}/minhook/src/hook.c ${TP}/minhook/src/trampoline.c
    ${TP}/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include)

add_library(infravr SHARED
    src/dllmain.cpp src/log.cpp src/config.cpp src/mathlib.cpp src/hooks.cpp src/game.cpp
    src/openvr_rt.cpp src/d3d_vr.cpp src/vr.cpp src/hooks_game.cpp src/d3d9.def)
set_target_properties(infravr PROPERTIES OUTPUT_NAME d3d9 PREFIX "")
target_include_directories(infravr PRIVATE src ${TP}/openvr ${TP}/vulkan)
target_compile_definitions(infravr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(infravr PRIVATE minhook psapi user32)
if(MSVC)
    set_property(TARGET infravr minhook PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(infravr PRIVATE /W3 /permissive-)
else()
    target_compile_options(infravr PRIVATE -O2 -Wall -Wno-unknown-pragmas -Wno-cast-function-type)
    target_link_options(infravr PRIVATE -s -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
endif()

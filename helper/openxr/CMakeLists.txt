# infravr_openxr.exe - the 64-bit OpenXR helper (OpenXR runtimes are 64-bit only; INFRA is 32-bit).
# Build separately from the mod:
#   cmake -S helper/openxr -B build-xr -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64-mingw.cmake -DCMAKE_BUILD_TYPE=Release
# or with Visual Studio: -A x64. It loads openxr_loader.dll from its own folder at run time
# (tools/fetch_openxr_loader.sh downloads Khronos' release build), so nothing links against it.
cmake_minimum_required(VERSION 3.16)
project(infravr_openxr LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The OpenXR helper must be built as 64-bit")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(infravr_openxr WIN32 main.cpp)
target_include_directories(infravr_openxr PRIVATE ${ROOT}/src/vr ${ROOT}/third_party/openxr)
target_compile_definitions(infravr_openxr PRIVATE _CRT_SECURE_NO_WARNINGS
    XR_NO_PROTOTYPES)
target_link_libraries(infravr_openxr PRIVATE d3d11 dxgi)
if(MSVC)
    set_property(TARGET infravr_openxr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    target_compile_options(infravr_openxr PRIVATE -O2 -Wall)
    target_link_options(infravr_openxr PRIVATE -s -static -static-libgcc -static-libstdc++)
endif()

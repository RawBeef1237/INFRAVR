// INFRA VR - common definitions shared by every translation unit.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

// Source (MSVC) member functions are __thiscall on x86: `this` in ECX, callee pops
// the stack arguments. Engine objects are never declared as C++ classes here (MSVC and
// MinGW vtable layouts can differ); they are called through raw function pointers.
// Detours use the __fastcall trick: ECX = this, EDX = unused, rest on the stack.
#define IVR_THISCALL __thiscall
#define IVR_FASTCALL __fastcall

// Return address of the current function (MSVC and GCC/MinGW spell it differently).
#ifdef _MSC_VER
#include <intrin.h>
#define IVR_RETURN_ADDRESS() _ReturnAddress()
#else
#define IVR_RETURN_ADDRESS() __builtin_return_address(0)
#endif

namespace ivr {

void LogInit(const char *path);
void Log(const char *fmt, ...);
void LogError(const char *fmt, ...);

std::string GameDir();   // folder with infra.exe, trailing backslash
std::string ModuleDir(); // folder with this d3d9.dll (INFRA\bin\), trailing backslash
// The mod folder INFRA\infravr\ (like Half-Life 2 VR's hlvr\ or Black Mesa VR's BMS\): cfg\,
// cfg\steamvr\, models\, bin\ and the logs. Trailing backslash.
std::string ModDir();
double NowSeconds();

} // namespace ivr

#define IVR_LOG(...) ::ivr::Log(__VA_ARGS__)
#define IVR_ERR(...) ::ivr::LogError(__VA_ARGS__)

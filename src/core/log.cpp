#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace ivr {

static std::mutex g_logMutex;
static FILE *g_logFile = nullptr;

void LogInit(const char *path)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile)
        return;
    g_logFile = _fsopen(path, "w", _SH_DENYWR);
}

static void LogV(const char *prefix, const char *fmt, va_list args)
{
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> lock(g_logMutex);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s%s\n", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, prefix, buffer);
    if (g_logFile)
    {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
    OutputDebugStringA(line);
}

void Log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV("", fmt, args);
    va_end(args);
}

void LogError(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV("ERROR: ", fmt, args);
    va_end(args);
}

std::string GameDir()
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
}

std::string ModuleDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ModuleDir), &self);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : s.substr(0, slash + 1);
}

double NowSeconds()
{
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(freq.QuadPart);
}

} // namespace ivr

#include "flags.h"
#include "config.h"
#include "hooks.h"

#include <cstdio>
#include <mutex>
#include <set>

namespace ivr {
namespace flags {

namespace {

std::mutex g_lock;
std::set<std::string> g_skipped;
std::set<std::string> g_active;
const char *volatile g_stage = "loading";
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
volatile LONG g_crashed = 0;

std::string SkipFile() { return ModDir() + "infravr_skip.txt"; }
std::string FlagFile(const std::string &name) { return ModDir() + "infravr_in_" + name + ".flag"; }

void AppendSkip(const std::string &name, const char *reason)
{
    if (FILE *f = fopen(SkipFile().c_str(), "a"))
    {
        fprintf(f, "%s ; %s\n", name.c_str(), reason);
        fclose(f);
    }
}

} // namespace

void Init()
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_Config.retrySkipped)
    {
        DeleteFileA(SkipFile().c_str());
        IVR_LOG("Flags: retry_skipped is on - every skipped mechanism will be tried again");
    }
    if (FILE *f = fopen(SkipFile().c_str(), "r"))
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            std::string s(line);
            size_t end = s.find_first_of(" ;\t\r\n");
            s = s.substr(0, end);
            if (!s.empty() && s[0] != '#')
                g_skipped.insert(s);
        }
        fclose(f);
    }
    // Leftover in-progress flags: INFRA closed while that mechanism was running.
    WIN32_FIND_DATAA fd;
    std::string pattern = ModDir() + "infravr_in_*.flag";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            std::string file = fd.cFileName;
            std::string name = file.substr(11, file.size() - 11 - 5); // infravr_in_<name>.flag
            DeleteFileA((ModDir() + file).c_str());
            if (g_skipped.insert(name).second)
            {
                AppendSkip(name, "INFRA closed or crashed while it was running");
                IVR_ERR("Flags: INFRA stopped during \"%s\" last time - it stays off from now on (delete its line "
                        "in infravr_skip.txt to try again)",
                        name.c_str());
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (const std::string &s : g_skipped)
        IVR_LOG("Flags: \"%s\" is skipped (infravr_skip.txt)", s.c_str());
}

bool Allowed(const char *name)
{
    std::lock_guard<std::mutex> lock(g_lock);
    return g_skipped.count(name) == 0;
}

bool Begin(const char *name)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_skipped.count(name))
        return false;
    if (g_active.insert(name).second)
    {
        if (FILE *f = fopen(FlagFile(name).c_str(), "w"))
            fclose(f);
        IVR_LOG("Flags: \"%s\" starts (crash-sticky)", name);
    }
    return true;
}

void End(const char *name)
{
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_active.erase(name))
    {
        DeleteFileA(FlagFile(name).c_str());
        IVR_LOG("Flags: \"%s\" is running fine", name);
    }
}

void Skip(const char *name, const char *reason)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_active.erase(name);
    DeleteFileA(FlagFile(name).c_str());
    if (g_skipped.insert(name).second)
    {
        AppendSkip(name, reason);
        IVR_ERR("Flags: \"%s\" turned off: %s", name, reason);
    }
}

void SetStage(const char *stage) { g_stage = stage; }
const char *Stage() { return g_stage; }

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    static volatile LONG s_once = 0;
    InterlockedExchange(&g_crashed, 1);
    if (InterlockedExchange(&s_once, 1) == 0 && ep && ep->ExceptionRecord)
    {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(addr), &mod))
            GetModuleFileNameA(mod, name, MAX_PATH);
        IVR_ERR("INFRA crashed: exception 0x%08lx at %p (%s +0x%lx) while the mod was at stage \"%s\"",
                (unsigned long)ep->ExceptionRecord->ExceptionCode, addr, name,
                mod ? (unsigned long)(static_cast<uint8_t *>(addr) - reinterpret_cast<uint8_t *>(mod)) : 0ul,
                g_stage);
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLogger()
{
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(&CrashFilter);
    if (prev != &CrashFilter) // installed again after the engine set its own: chain to that one
        g_prevFilter = prev;
}

void OnProcessExit()
{
    if (g_crashed)
        return;
    // Other threads are already gone at process exit and may have died holding the lock.
    if (!g_lock.try_lock())
        return;
    for (const std::string &name : g_active)
    {
        DeleteFileA(FlagFile(name).c_str());
        IVR_LOG("Flags: INFRA closed normally while \"%s\" was still on trial - it stays on", name.c_str());
    }
    g_active.clear();
    g_lock.unlock();
}

namespace {
// INFRA's quit (also the phone menu's Quit) ends with the engine's Plat_ExitProcess, which calls
// TerminateProcess on itself: DLL_PROCESS_DETACH never comes, and every mechanism still on trial was
// taken for a crash on the next launch. A crash the filter saw still counts as one.
using TerminateProcessFn = BOOL(WINAPI *)(HANDLE, UINT);
Hook<TerminateProcessFn> g_hkTerminate;

BOOL WINAPI TerminateProcessDetour(HANDLE process, UINT code)
{
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
    {
        IVR_LOG("INFRA is closing (TerminateProcess, code %u)%s", code, g_crashed ? " after a crash" : "");
        OnProcessExit();
    }
    return g_hkTerminate.original(process, code);
}
} // namespace

void InstallExitHook()
{
    if (g_hkTerminate.Installed())
        return;
    HMODULE k = GetModuleHandleA("kernelbase.dll");
    void *fn = k ? reinterpret_cast<void *>(GetProcAddress(k, "TerminateProcess")) : nullptr;
    if (!fn && (k = GetModuleHandleA("kernel32.dll")))
        fn = reinterpret_cast<void *>(GetProcAddress(k, "TerminateProcess"));
    g_hkTerminate.Install("TerminateProcess", fn, reinterpret_cast<void *>(&TerminateProcessDetour));
}

} // namespace flags
} // namespace ivr

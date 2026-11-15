#include "cpu_usage.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>

namespace ivr {
namespace cpu {

namespace {

constexpr double kInterval = 15.0;   // seconds between reports
constexpr double kBusy = 0.10;       // a thread that used at least this much of one core is "busy"
constexpr int kMaxProcs = 64;        // processors tracked per role (two 32-bit masks)

typedef DWORD(WINAPI *GetCurrentProcessorNumberFn)();
GetCurrentProcessorNumberFn g_getProc = nullptr;
bool g_getProcLooked = false;

volatile LONG g_procMask[RoleCount][2] = {};

std::map<DWORD, uint64_t> g_prevThreadTime; // thread id -> kernel + user time (100 ns)
double g_prevWall = 0;
uint64_t g_prevProcessTime = 0;
bool g_firstReport = true;

uint64_t FileTime64(const FILETIME &ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

int CountBits(uint64_t v)
{
    int n = 0;
    for (; v; v &= v - 1)
        ++n;
    return n;
}

uint64_t ProcessCpuTime()
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0;
    return FileTime64(k) + FileTime64(u);
}

// CPU time (kernel + user, 100 ns units) of every thread in this process right now.
std::map<DWORD, uint64_t> ThreadTimes()
{
    std::map<DWORD, uint64_t> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te))
    {
        do
        {
            if (te.dwSize < offsetof(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID) ||
                te.th32OwnerProcessID != pid)
                continue;
            // THREAD_QUERY_LIMITED_INFORMATION (0x0800) first, THREAD_QUERY_INFORMATION as fallback.
            HANDLE th = OpenThread(0x0800, FALSE, te.th32ThreadID);
            if (!th)
                th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(th, &c, &e, &k, &u))
                out[te.th32ThreadID] = FileTime64(k) + FileTime64(u);
            CloseHandle(th);
        } while (te.dwSize = sizeof(te), Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return out;
}

uint64_t TakeMask(Role role)
{
    uint64_t lo = (uint32_t)InterlockedExchange(&g_procMask[role][0], 0);
    uint64_t hi = (uint32_t)InterlockedExchange(&g_procMask[role][1], 0);
    return lo | (hi << 32);
}

std::string MaskList(uint64_t mask)
{
    if (!mask)
        return g_getProc ? "?" : "n/a";
    std::string s;
    for (int i = 0; i < kMaxProcs; ++i)
        if (mask & ((uint64_t)1 << i))
        {
            if (!s.empty())
                s += ",";
            s += std::to_string(i);
        }
    return s;
}

int LogicalProcessors()
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

int AffinityProcessors()
{
    DWORD_PTR proc = 0, sys = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        return 0;
    return CountBits((uint64_t)proc);
}

} // namespace

void NoteProcessor(Role role)
{
    if (!g_getProcLooked)
    {
        // Looked up at run time: older MinGW headers only declare it for newer _WIN32_WINNT.
        g_getProc = reinterpret_cast<GetCurrentProcessorNumberFn>(
            reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetCurrentProcessorNumber")));
        g_getProcLooked = true;
    }
    if (!g_getProc || role < 0 || role >= RoleCount)
        return;
    DWORD p = g_getProc();
    if (p >= (DWORD)kMaxProcs)
        return;
    InterlockedOr(&g_procMask[role][p / 32], (LONG)(1u << (p % 32)));
}

void Restart()
{
    g_prevThreadTime = ThreadTimes();
    g_prevProcessTime = ProcessCpuTime();
    g_prevWall = NowSeconds();
    TakeMask(MainThread);
    TakeMask(RenderThread);
}

void Report(int queueMode, bool multicoreWanted, DWORD mainTid, DWORD renderTid)
{
    double now = NowSeconds();
    if (g_prevWall == 0)
    {
        Restart();
        return;
    }
    if (now - g_prevWall < kInterval)
        return;

    std::map<DWORD, uint64_t> times = ThreadTimes();
    uint64_t processTime = ProcessCpuTime();
    double wall100ns = (now - g_prevWall) * 1e7;
    if (wall100ns <= 0)
        return;

    struct Busy
    {
        DWORD tid;
        double cores;
    };
    std::vector<Busy> busy;
    double mainUse = 0, renderUse = 0;
    for (const auto &t : times)
    {
        auto prev = g_prevThreadTime.find(t.first);
        uint64_t before = prev != g_prevThreadTime.end() ? prev->second : 0;
        double use = t.second >= before ? (double)(t.second - before) / wall100ns : 0.0;
        if (t.first == mainTid)
            mainUse = use;
        if (t.first == renderTid)
            renderUse = use;
        if (use >= kBusy)
            busy.push_back({t.first, use});
    }
    std::sort(busy.begin(), busy.end(), [](const Busy &a, const Busy &b) { return a.cores > b.cores; });
    double total = processTime >= g_prevProcessTime ? (double)(processTime - g_prevProcessTime) / wall100ns : 0.0;

    uint64_t mainMask = TakeMask(MainThread);
    uint64_t renderMask = TakeMask(RenderThread);
    bool split = mainTid && renderTid && mainTid != renderTid;

    if (g_firstReport)
    {
        g_firstReport = false;
        IVR_LOG("CPU: %d logical processors, %d usable by INFRA (process affinity). Reports every %.0f s while "
                "in a level; \"busy\" = a thread that used at least %.0f%% of one core.",
                LogicalProcessors(), AffinityProcessors(), kInterval, kBusy * 100);
    }

    std::string others;
    int shown = 0;
    for (const Busy &b : busy)
    {
        if (b.tid == mainTid || b.tid == renderTid)
            continue;
        if (shown++ == 4)
        {
            others += " ...";
            break;
        }
        char buf[48];
        snprintf(buf, sizeof(buf), " %lu:%.0f%%", (unsigned long)b.tid, b.cores * 100);
        others += buf;
    }

    char renderPart[160];
    if (split)
        snprintf(renderPart, sizeof(renderPart), "render thread %lu %.0f%% on cpu %s", (unsigned long)renderTid,
                 renderUse * 100, MaskList(renderMask).c_str());
    else
        snprintf(renderPart, sizeof(renderPart), "Present on the main thread (no separate render thread)");

    IVR_LOG("CPU: %.2f cores' worth in use, %d busy thread(s); main thread %lu %.0f%% on cpu %s; %s; "
            "other busy:%s; cpus used by main+render: %d [mat_queue_mode %d, multicore option %s]",
            total, (int)busy.size(), (unsigned long)mainTid, mainUse * 100, MaskList(mainMask).c_str(), renderPart,
            others.empty() ? " none" : others.c_str(), CountBits(mainMask | renderMask), queueMode,
            multicoreWanted ? "on" : "off");

    if (queueMode == 2)
    {
        if (split && renderUse >= kBusy)
            IVR_LOG("CPU: multicore rendering is working - the engine and the render thread run in parallel "
                    "(%.0f%% + %.0f%% of a core)",
                    mainUse * 100, renderUse * 100);
        else
            IVR_LOG("CPU: multicore rendering was asked for but %s",
                    split ? "the render thread is nearly idle" : "Present still runs on the main thread");
    }

    g_prevThreadTime.swap(times);
    g_prevProcessTime = processTime;
    g_prevWall = now;
}

} // namespace cpu
} // namespace ivr

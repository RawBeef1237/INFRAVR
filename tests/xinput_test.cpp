// The virtual pad must answer through INFRA's own bin\xinput1_3.dll (loaded by full path) and
// through the system copy.
#include <windows.h>
#include <xinput.h>
#include <cstdio>
#include <cstring>
namespace ivr { void InstallGamepadHooks(); }
static bool Check(HMODULE x, const char *what)
{
    auto gs = (DWORD(WINAPI *)(DWORD, XINPUT_STATE *))GetProcAddress(x, "XInputGetState");
    auto gc = (DWORD(WINAPI *)(DWORD, DWORD, XINPUT_CAPABILITIES *))GetProcAddress(x, "XInputGetCapabilities");
    XINPUT_STATE s{}; XINPUT_CAPABILITIES c{};
    DWORD a = gc(0, 0, &c), b = gs(0, &s), d = gs(1, &s);
    bool ok = a == 0 && b == 0 && d != 0;
    printf("%s: caps %lu type %d, state0 %lu, state1 %lu -> %s\n", what, a, c.Type, b, d, ok ? "ok" : "bad");
    return ok;
}
int main()
{
    ivr::InstallGamepadHooks();
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "xinput1_3.dll");
    HMODULE bin = LoadLibraryA(path), sys = LoadLibraryA("xinput1_3.dll");
    bool ok = bin && Check(bin, "bin copy") && Check(sys, "by name");
    printf("%s\n", ok ? "PASSED" : "FAILED");
}

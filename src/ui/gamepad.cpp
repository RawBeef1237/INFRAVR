// INFRA VR - the VR controllers as an Xbox controller for INFRA's own gamepad support.
//
// While the phone menu is open INFRA gets a virtual controller in slot 1, driven by the VR
// controllers exactly like a gamepad: right stick = right stick, left stick = left stick, A = A,
// B = B and so on, so the menu behaves as it does with a real controller. The rest of the time
// the virtual controller is centred and idle (a real controller, if plugged in, passes through).
#include "gamepad.h"
#include "config.h"
#include "hooks.h"
#include "mod.h"

#include <xinput.h>
#include <algorithm>
#include <cstring>

namespace ivr {
namespace {

using GetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
using GetCapsFn = DWORD(WINAPI *)(DWORD, DWORD, XINPUT_CAPABILITIES *);
// INFRA loads its own copy (bin\xinput1_3.dll, by full path) next to the system one, so each
// copy gets its own pair of hooks.
constexpr int kCopies = 2;
Hook<GetStateFn> g_hkGetState[kCopies];
Hook<GetCapsFn> g_hkGetCaps[kCopies];
HMODULE g_hooked[kCopies] = {};
DWORD g_packet = 0;
XINPUT_GAMEPAD g_last{};
bool g_loggedUse = false;

SHORT Axis(float v)
{
    v = std::max(-1.0f, std::min(1.0f, v));
    return (SHORT)(v * 32767.0f);
}

// The VR controllers as a gamepad (only while the phone menu is open).
bool VirtualPad(XINPUT_GAMEPAD &g)
{
    g = XINPUT_GAMEPAD{};
    if (!g_Config.phoneGamepad || !g_Mod.phoneMenu || !g_VR.Ok())
        return false;
    const InputState &in = g_VR.Input();
    g.sThumbLX = Axis(in.move.x);
    g.sThumbLY = Axis(in.move.y);
    g.sThumbRX = Axis(in.turn.x);
    g.sThumbRY = Axis(in.turn.y);
    if (in.jump.state)
        g.wButtons |= XINPUT_GAMEPAD_A;
    if (in.reload.state)
        g.wButtons |= XINPUT_GAMEPAD_B;
    if (in.sprint.state)
        g.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (in.crouch.state)
        g.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (in.useRight.state)
        g.bRightTrigger = 255;
    if (in.useLeft.state)
        g.bLeftTrigger = 255;
    return true;
}

DWORD GetState(int copy, DWORD user, XINPUT_STATE *state)
{
    DWORD r = g_hkGetState[copy].original(user, state);
    if (user != 0 || !state)
        return r;
    static bool s_polled = false;
    if (!s_polled)
    {
        s_polled = true;
        IVR_LOG("XInput: INFRA reads controller 1");
    }
    XINPUT_GAMEPAD pad;
    if (VirtualPad(pad))
    {
        if (!g_loggedUse)
        {
            g_loggedUse = true;
            IVR_LOG("Phone menu: the VR controllers act as INFRA's gamepad");
        }
    }
    else if (r == ERROR_SUCCESS)
        return r; // a real controller, outside the phone menu
    if (memcmp(&pad, &g_last, sizeof(pad)) != 0)
    {
        g_last = pad;
        ++g_packet;
    }
    state->dwPacketNumber = g_packet;
    state->Gamepad = pad;
    return ERROR_SUCCESS;
}

DWORD GetCaps(int copy, DWORD user, DWORD flags, XINPUT_CAPABILITIES *caps)
{
    DWORD r = g_hkGetCaps[copy].original(user, flags, caps);
    static bool s_logged = false;
    if (user == 0 && !s_logged)
    {
        s_logged = true;
        IVR_LOG("XInput: INFRA looked for controller 1 (%s)", r == ERROR_SUCCESS ? "a real one is plugged in" : "using the virtual one");
    }
    if (user != 0 || !caps || r == ERROR_SUCCESS)
        return r;
    *caps = XINPUT_CAPABILITIES{};
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xFFC0;
    return ERROR_SUCCESS;
}

template <int N>
DWORD WINAPI hkXInputGetState(DWORD user, XINPUT_STATE *state)
{
    return GetState(N, user, state);
}

template <int N>
DWORD WINAPI hkXInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES *caps)
{
    return GetCaps(N, user, flags, caps);
}

template <int N>
void HookCopy(HMODULE x, const char *path)
{
    if (!x)
        return;
    for (HMODULE h : g_hooked)
        if (h == x)
            return; // the same module under another name
    g_hooked[N] = x;
    g_hkGetState[N].Install("XInputGetState", reinterpret_cast<void *>(GetProcAddress(x, "XInputGetState")),
                            reinterpret_cast<void *>(&hkXInputGetState<N>));
    g_hkGetCaps[N].Install("XInputGetCapabilities",
                           reinterpret_cast<void *>(GetProcAddress(x, "XInputGetCapabilities")),
                           reinterpret_cast<void *>(&hkXInputGetCapabilities<N>));
    IVR_LOG("XInput: hooked %s", path);
}

} // namespace

void InstallGamepadHooks()
{
    if (!g_Config.phoneGamepad || g_hooked[0] || g_hooked[1])
        return;
    HooksInit();
    // INFRA's inputsystem loads bin\xinput1_3.dll by full path; we live in bin too.
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&InstallGamepadHooks), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    if (char *slash = strrchr(path, '\\'))
    {
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "xinput1_3.dll");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            HookCopy<0>(LoadLibraryA(path), path);
    }
    HookCopy<1>(LoadLibraryA("xinput1_3.dll"), "xinput1_3.dll");
    if (!g_hooked[0] && !g_hooked[1])
        IVR_ERR("xinput1_3.dll not found - the phone menu cannot use the VR controllers as a gamepad");
}

} // namespace ivr

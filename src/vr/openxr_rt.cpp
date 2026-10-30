#include "openxr_rt.h"
#include "config.h"
#include "flags.h"
#include "overlay_api.h"
#include "runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ivr {
namespace xr {

bool Link::Alive() const
{
    if (!shm || shm->helperState != 1)
        return false;
    return !process || WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

void Stop(Link *link)
{
    if (!link)
        return;
    if (link->shm)
    {
        link->shm->gameQuit = 1;
        UnmapViewOfFile(link->shm);
    }
    if (link->frameEvent)
        CloseHandle(link->frameEvent);
    if (link->map)
        CloseHandle(link->map);
    if (link->process)
        CloseHandle(link->process);
    delete link;
}

Link *Start(double timeoutSeconds)
{
    std::string exe = ModuleDir() + "infravr\\openxr\\infravr_openxr.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        IVR_ERR("OpenXR: %s is missing", exe.c_str());
        return nullptr;
    }
    DWORD pid = GetCurrentProcessId();
    char name[96];
    Link *link = new Link();
    snprintf(name, sizeof(name), IVRXR_SHM_NAME "%lu", (unsigned long)pid);
    link->map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ivrxr::Shared), name);
    if (link->map)
        link->shm = static_cast<ivrxr::Shared *>(MapViewOfFile(link->map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ivrxr::Shared)));
    snprintf(name, sizeof(name), IVRXR_FRAME_EVENT "%lu", (unsigned long)pid);
    link->frameEvent = CreateEventA(nullptr, FALSE, FALSE, name);
    if (!link->shm || !link->frameEvent)
    {
        IVR_ERR("OpenXR: could not create the shared memory (error %lu)", GetLastError());
        Stop(link);
        return nullptr;
    }
    std::memset(link->shm, 0, sizeof(ivrxr::Shared));
    link->shm->magic = ivrxr::kMagic;
    link->shm->version = ivrxr::kVersion;
    link->shm->gamePid = pid;
    link->shm->helperUsingSlot = 0xFFFFFFFFu;

    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "\"%s\" %lu", exe.c_str(), (unsigned long)pid);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string dir = ModuleDir() + "infravr\\openxr";
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi))
    {
        IVR_ERR("OpenXR: could not start %s (error %lu)", exe.c_str(), GetLastError());
        Stop(link);
        return nullptr;
    }
    CloseHandle(pi.hThread);
    link->process = pi.hProcess;
    link->shm->helperPid = pi.dwProcessId;

    double until = NowSeconds() + timeoutSeconds;
    while (link->shm->helperState == 0 && NowSeconds() < until)
    {
        if (WaitForSingleObject(link->process, 50) != WAIT_TIMEOUT)
            break;
    }
    if (link->shm->helperState != 1)
    {
        IVR_ERR("OpenXR helper did not come up (state %u%s%s) - see infravr_openxr.log",
                (unsigned)link->shm->helperState, link->shm->error[0] ? ": " : "", link->shm->error);
        TerminateProcess(link->process, 1);
        Stop(link);
        return nullptr;
    }
    IVR_LOG("OpenXR helper ready: runtime \"%s\", %ux%u per eye", link->shm->runtimeName,
            link->shm->recommendedWidth, link->shm->recommendedHeight);
    return link;
}

} // namespace xr

// --- VrRuntime, OpenXR backend -------------------------------------------------------------------

static vr::HmdMatrix34_t ToHmd(const float m[3][4])
{
    vr::HmdMatrix34_t out;
    std::memcpy(out.m, m, sizeof(out.m));
    return out;
}

bool VrRuntime::InitOpenXR()
{
    if (!flags::Begin("openxr_start"))
        return false;
    m_xr = xr::Start(20.0);
    if (!m_xr)
    {
        flags::End("openxr_start");
        return false;
    }
    flags::End("openxr_start");
    ivrxr::Shared *s = m_xr->shm;
    // Runtime tangents -> the OpenVR raw-projection convention SetProjection uses (see there).
    float l[2], r[2], t[2], b[2];
    for (int e = 0; e < 2; ++e)
    {
        l[e] = s->fov[e][0];
        r[e] = s->fov[e][1];
        t[e] = s->fov[e][3];
        b[e] = s->fov[e][2];
        eyeOffset[e] = ConvertVector(s->eyeToHead[e][0], s->eyeToHead[e][1], s->eyeToHead[e][2]);
    }
    SetProjection(l, r, t, b, s->recommendedWidth, s->recommendedHeight);
    // The helper copies the whole view; it is told the symmetric frustum the game draws.
    float tanX = std::tan(Deg2Rad(fovX * 0.5f)), tanY = tanX / aspect;
    s->renderFov[0] = -tanX;
    s->renderFov[1] = tanX;
    s->renderFov[2] = tanY;
    s->renderFov[3] = -tanY;
    m_overlays = MakeOpenXROverlays(m_xr);
    m_backend = Backend::OpenXR;
    return true;
}

void VrRuntime::WaitPosesOpenXR()
{
    if (!m_xr || !m_xr->shm)
        return;
    // The helper signals once per headset frame (after xrWaitFrame): this is the frame pacing.
    WaitForSingleObject(m_xr->frameEvent, 40);
    ivrxr::Shared *s = m_xr->shm;
    ivrxr::Pose head{}, hands[2]{};
    float move[2]{}, turn[2]{}, trig[2]{}, sq[2]{};
    uint32_t buttons = 0;
    for (int tries = 0; tries < 64; ++tries)
    {
        uint32_t a = s->poseSeq;
        if (a & 1)
        {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        head = s->head;
        hands[0] = s->hands[0];
        hands[1] = s->hands[1];
        std::memcpy(move, s->move, sizeof(move));
        std::memcpy(turn, s->turn, sizeof(turn));
        std::memcpy(trig, s->trigger, sizeof(trig));
        std::memcpy(sq, s->squeeze, sizeof(sq));
        buttons = s->buttons;
        MemoryBarrier();
        if (s->poseSeq == a)
        {
            m_xr->lastPoseSeq = a;
            break;
        }
    }
    if (head.valid)
    {
        m_head = ConvertPose(ToHmd(head.m));
        m_havePoses = true;
    }
    for (int h = 0; h < 2; ++h)
    {
        if (hands[h].valid)
        {
            m_hands[h] = ConvertPose(ToHmd(hands[h].m));
            m_hands[h].velocity = ConvertVector(hands[h].velocity[0], hands[h].velocity[1], hands[h].velocity[2]);
        }
        else
            m_hands[h].valid = false;
        // No skeleton in core OpenXR: the hand closes with the grip and the index finger follows
        // the trigger.
        skeletonValid[h] = true;
        fingerCurl[h][0] = std::max(sq[h], trig[h]) * 0.7f;
        fingerCurl[h][1] = trig[h];
        fingerCurl[h][2] = fingerCurl[h][3] = fingerCurl[h][4] = sq[h];
    }
    auto D = [&](DigitalAction &d, uint32_t bit) {
        bool prev = d.state;
        d.state = (buttons & bit) != 0;
        d.pressed = d.state && !prev;
        d.released = !d.state && prev;
    };
    D(m_input.jump, ivrxr::BTN_JUMP);
    D(m_input.crouch, ivrxr::BTN_CROUCH);
    D(m_input.sprint, ivrxr::BTN_SPRINT);
    D(m_input.useRight, ivrxr::BTN_USE_RIGHT);
    D(m_input.useLeft, ivrxr::BTN_USE_LEFT);
    D(m_input.gripLeft, ivrxr::BTN_GRIP_LEFT);
    D(m_input.gripRight, ivrxr::BTN_GRIP_RIGHT);
    D(m_input.flashlight, ivrxr::BTN_FLASHLIGHT);
    D(m_input.phone, ivrxr::BTN_PHONE);
    D(m_input.reload, ivrxr::BTN_RELOAD);
    D(m_input.pause, ivrxr::BTN_PAUSE);
    D(m_input.recenter, ivrxr::BTN_RECENTER);
    m_input.move.x = move[0];
    m_input.move.y = move[1];
    m_input.turn.x = turn[0];
    m_input.turn.y = turn[1];
}

void VrRuntime::UpdateInputOpenXR()
{
    // Everything is read together with the poses in WaitPosesOpenXR (one consistent snapshot).
}

void VrRuntime::HapticOpenXR(int hand, float seconds, float frequency, float amplitude)
{
    if (!m_xr || !m_xr->shm || !g_Config.haptics)
        return;
    ivrxr::Shared *s = m_xr->shm;
    s->hapticSeconds[hand] = seconds;
    s->hapticFrequency[hand] = frequency;
    s->hapticAmplitude[hand] = amplitude;
    MemoryBarrier();
    s->hapticSeq[hand] = s->hapticSeq[hand] + 1;
}

} // namespace ivr

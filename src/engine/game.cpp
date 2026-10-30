#include "game.h"
#include "world_size.h"
#include "hooks.h"
#include "config.h"

#include <algorithm>
#include <map>

namespace ivr {

Game g_Game;

namespace {

struct FnSpec
{
    const char *what;
    const char *module;
    uint32_t rva;
    const char *pattern;
};

// RVAs are INFRA's (PatchVersion 3.3.3); the patterns are checked at that address first
// and scanned for as a fallback. Relocated bytes and call targets are wildcarded.
const FnSpec kRenderView = {"CViewRender::RenderView", "client.dll", 0x198e90,
                            "55 8B EC 83 EC 2C 53 56 8B F1 6A 00 8D 8E 28 02 00 00"};
const FnSpec kRenderCameraWeapon = {"CViewRender::RenderCameraWeapon", "client.dll", 0x198820,
                                    "55 8B EC 81 EC 74 05 00 00 53 56 8B 75 08 57 89 4D F8"};
const FnSpec kCreateMove = {"ClientModeShared::CreateMove", "client.dll", 0x0a4910,
                            "55 8B EC 6A FF E8 ? ? ? ? 83 C4 04 85 C0 75 06 B0 01"};
const FnSpec kCalcViewModelView = {"C_BasePlayer::CalcViewModelView", "client.dll", 0x1ea9e0,
                                   "55 8B EC 83 EC 34 53 8B D9 80 BB DC 1C 00 00 00"};
const FnSpec kFlashlightUpdate = {"CFlashlightEffect::UpdateLight", "client.dll", 0x0cb160,
                                  "55 8B EC 81 EC 00 01 00 00 53 56 8B F1 8B 0D ? ? ? ?"};
// INFRA's player flashlight (uses the infra_flashlight_* convars): thiscall
// Update(Vector origin, Quaternion orientation), both by value; fills a FlashlightState_t from them.
const FnSpec kInfraFlashlightUpdate = {"INFRA flashlight update", "client.dll", 0x1afb60,
                                       "55 8B EC 81 EC 60 01 00 00 80 39 00 89 4D D8 0F 84 ? ? ? ? 56 6A FF"};
// C_BaseAnimating::BuildTransformations(CStudioHdr *, Vector *, Quaternion *, const matrix3x4_t &, int,
// CBoneBitList &) - C_BaseAnimating vtable 178, VPROF "C_BaseAnimating::BuildTransformations".
const FnSpec kBuildTransformations = {"C_BaseAnimating::BuildTransformations", "client.dll", 0x02b110,
                                      "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC "
                                      "81 EC 98 01 00 00 80 3D ? ? ? ? 00 56 57 8B F1"};
const FnSpec kUpdateObject = {"CGrabController::UpdateObject", "server.dll", 0x3a9820,
                              "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC E8 02 00 00 "
                              "56 57 8B F9 8B 87 54 20 00 00"};
const FnSpec kUpdateObjectVM = {"CGrabController::UpdateObjectVM", "server.dll", 0x3ab320,
                                "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC D8 01 00 "
                                "00 56 57"};
const FnSpec kComputeError = {"CGrabController::ComputeError", "server.dll", 0x3a7740,
                              "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 18 01 00 00 "
                              "56 8B F1 8B 86 94 20 00 00"};
const FnSpec kEyeAngles = {"CBasePlayer::EyeAngles", "server.dll", 0x0d78f0,
                           "55 8B EC 8B 81 DC 00 00 00 83 EC 60 56 57 8B 3D ? ? ? ?"};
// CBasePlayer::Weapon_ShootPosition (vtable 290, "return EyePosition()") - the start point
// CGrabController::UpdateObject holds objects in front of.
const FnSpec kShootPosition = {"CBasePlayer::Weapon_ShootPosition", "server.dll", 0x34bbc0,
                               "55 8B EC 8B 01 8B 90 00 02 00 00 56 8B 75 08 56 FF D2 8B C6 5E 5D C2 04 00"};

void *Resolve(const FnSpec &s) { return ResolveFunction(s.what, s.module, s.rva, s.pattern); }

void *Require(const char *module, const char *name)
{
    void *p = GetInterface(module, name);
    if (p)
        IVR_LOG("%s!%s = %p", module, name, p);
    else
        IVR_ERR("%s!%s not found", module, name);
    return p;
}

} // namespace

bool Game::Init()
{
    engineClient = Require("engine.dll", "VEngineClient015");
    renderView = Require("engine.dll", "VEngineRenderView013");
    entityList = Require("client.dll", "VClientEntityList003");
    materials = Require("materialsystem.dll", "VMaterialSystem080");
    surface = Require("vguimatsurface.dll", "VGUI_Surface031");
    vguiInput = Require("vgui2.dll", "VGUI_InputInternal001");
    if (!engineClient || !renderView || !entityList || !materials)
        return false;

    // CMaterialSystem::BeginRenderTargetAllocation starts with "cmp byte ptr [ecx+disp32], 0"
    const uint8_t *begin = static_cast<const uint8_t *>(VTable(materials)[vidx::BeginRenderTargetAllocation]);
    if (begin[0] == 0x80 && begin[1] == 0xB9 && begin[6] == 0x00)
        matGameStartedFlag = *reinterpret_cast<const uint32_t *>(begin + 2);
    IVR_LOG("Material system 'game started' flag at +0x%X", (unsigned)matGameStartedFlag);

    fnRenderView = Resolve(kRenderView);
    fnRenderCameraWeapon = Resolve(kRenderCameraWeapon);
    fnCreateMove = Resolve(kCreateMove);
    fnCalcViewModelView = Resolve(kCalcViewModelView);
    fnFlashlightUpdate = Resolve(kFlashlightUpdate);

    // Cross-check the RVAs against the RTTI vtables where the function is virtual.
    if (void **vt = FindVTable("client.dll", ".?AVClientModeShared@@"))
    {
        if (vt[24] != fnCreateMove)
            IVR_LOG("ClientModeShared vtable[24] = %p (signature gave %p) - using vtable", vt[24], fnCreateMove);
        fnCreateMove = vt[24];
    }
    if (void **vt = FindVTable("client.dll", ".?AVC_INFRA_Player@@"))
    {
        if (vt[248] != fnCalcViewModelView)
            IVR_LOG("C_INFRA_Player vtable[248] = %p (signature gave %p)", vt[248], fnCalcViewModelView);
    }
    fnInfraFlashlightUpdate = Resolve(kInfraFlashlightUpdate);
    fnBuildTransformations = Resolve(kBuildTransformations);
    if (void **vt = FindVTable("client.dll", ".?AVC_BaseAnimating@@"))
    {
        if (vt[178] != fnBuildTransformations)
            IVR_LOG("C_BaseAnimating vtable[178] = %p (signature gave %p) - using vtable", vt[178],
                    fnBuildTransformations);
        fnBuildTransformations = vt[178];
    }
    if (fnBuildTransformations)
    {
        // "lea edi, [esi+m_BoneAccessor]" near the start; m_pBones follows m_pAnimating.
        const uint8_t *p = static_cast<const uint8_t *>(fnBuildTransformations);
        for (int i = 0; i < 0x180 && !boneArrayOffset; ++i)
            if (p[i] == 0x8D && p[i + 1] == 0xBE)
            {
                int disp = *reinterpret_cast<const int *>(p + i + 2);
                if (disp > 0x400 && disp < 0x2000)
                    boneArrayOffset = disp + 4;
            }
        IVR_LOG("Bone array at +0x%X", (unsigned)boneArrayOffset);
        if (!boneArrayOffset)
            fnBuildTransformations = nullptr;
    }
    FindScreenClicker();
    ready = fnRenderView && fnCreateMove;
    return ready;
}

bool Game::InitServer()
{
    if (!GetModuleHandleA("server.dll"))
        return false;
    fnGrabComputeError = Resolve(kComputeError);
    if (void **vt = FindVTable("server.dll", ".?AVCGrabController@@"))
    {
        fnGrabUpdateObject = vt[5];
        fnGrabUpdateObjectVM = vt[6];
    }
    else
    {
        fnGrabUpdateObject = Resolve(kUpdateObject);
        fnGrabUpdateObjectVM = Resolve(kUpdateObjectVM);
    }
    if (void **vt = FindVTable("server.dll", ".?AVCINFRA_Player@@"))
    {
        fnPlayerEyeAngles = vt[129];
        fnPlayerShootPosition = vt[290];
    }
    else
    {
        fnPlayerEyeAngles = Resolve(kEyeAngles);
        fnPlayerShootPosition = Resolve(kShootPosition);
    }
    IVR_LOG("server: UpdateObject %p, UpdateObjectVM %p, ComputeError %p, EyeAngles %p, Weapon_ShootPosition %p",
            fnGrabUpdateObject, fnGrabUpdateObjectVM, fnGrabComputeError, fnPlayerEyeAngles, fnPlayerShootPosition);
    return fnGrabUpdateObject && fnPlayerEyeAngles && fnPlayerShootPosition;
}

void *Game::LocalPlayer() const
{
    int idx = LocalPlayerIndex();
    if (idx <= 0 || !entityList)
        return nullptr;
    void *ent = VCall<void *>(entityList, vidx::GetClientEntity, idx);
    return LooksLikeObject(ent) ? ent : nullptr;
}

void *Game::ActiveWeapon() const
{
    void *player = LocalPlayer();
    if (!player)
        return nullptr;
    void *weapon = VCall<void *>(player, vidx::GetActiveWeapon);
    return LooksLikeObject(weapon) ? weapon : nullptr;
}

void Game::ExecuteCommand(const char *cmd) const
{
    if (!engineClient || !cmd || !*cmd)
        return;
    IVR_LOG("> %s", cmd);
    VCall<void, const char *>(engineClient, vidx::ExecuteClientCmd, cmd);
}

namespace {

// ConCommandBase layout (Portal 2 / Alien Swarm branch, same as SDK 2013):
// +0 vtable, +4 m_pNext, +8 m_bRegistered, +12 m_pszName, +16 m_pszHelpString, +20 m_nFlags
constexpr int FCVAR_DEVELOPMENTONLY = 1 << 1;
constexpr int FCVAR_CHEAT = 1 << 14;

struct Section
{
    uint8_t *begin, *end;
    bool writable;
};

std::vector<Section> ReadableSections(const ModuleInfo &mi)
{
    std::vector<Section> out;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(mi.base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(mi.base + dos->e_lfanew);
    auto *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) || (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t *b = mi.base + sec->VirtualAddress;
        uint8_t *e = b + sec->Misc.VirtualSize;
        bool writable = (sec->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        // A section can span several memory regions; keep the readable ones.
        while (b < e)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(b, &mbi, sizeof(mbi)))
                break;
            uint8_t *regionEnd = static_cast<uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
            uint8_t *stop = std::min(regionEnd, e);
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                out.push_back({b, stop, writable});
            b = stop;
        }
    }
    return out;
}

uint8_t *FindConVar(const char *module, const char *name)
{
    ModuleInfo mi = GetModuleInfo(module);
    if (!mi.base)
        return nullptr;
    std::vector<Section> secs = ReadableSections(mi);
    size_t len = strlen(name) + 1; // include the terminator
    for (const Section &str : secs)
        for (uint8_t *p = str.begin + 1; p + len <= str.end; ++p)
        {
            if (*p != (uint8_t)name[0] || p[-1] != 0 || memcmp(p, name, len) != 0)
                continue;
            uint32_t target = (uint32_t)(uintptr_t)p;
            for (const Section &data : secs)
            {
                if (!data.writable)
                    continue;
                for (uint8_t *q = data.begin + 12; q + 12 <= data.end; q += 4)
                {
                    if (*reinterpret_cast<uint32_t *>(q) != target)
                        continue;
                    uint8_t *obj = q - 12;
                    uint8_t *vt = *reinterpret_cast<uint8_t **>(obj);
                    if (vt < mi.base || vt >= mi.base + mi.size) // a ConVar's vtable is in the same module
                        continue;
                    return obj;
                }
            }
        }
    return nullptr;
}

} // namespace

std::string Game::ConVarString(const char *name) const
{
    static std::map<std::string, uint8_t *> cache;
    auto it = cache.find(name);
    uint8_t *cv = nullptr;
    if (it == cache.end())
    {
        for (const char *module : {"engine.dll", "client.dll", "materialsystem.dll"})
            if ((cv = FindConVar(module, name)) != nullptr)
                break;
        if (cv)
        {
            uint8_t *parent = *reinterpret_cast<uint8_t **>(cv + 28);
            if (parent && parent != cv && IsReadable(parent, 40))
                cv = parent;
        }
        cache[name] = cv;
    }
    else
        cv = it->second;
    if (!cv || !IsReadable(cv + 36, 4))
        return "";
    const char *s = *reinterpret_cast<const char **>(cv + 36);
    if (!s || !IsReadable(s, 1))
        return "";
    std::string out;
    for (int i = 0; i < 260 && IsReadable(s + i, 1) && s[i]; ++i)
        out += s[i];
    return out;
}

void Game::ForceConVar(const char *name, const char *value) const
{
    // ConVar: +28 m_pParent, +36 m_pszString (current value, kept in the parent)
    static std::map<std::string, uint8_t *> cache; // the module scan is slow-ish: once per name
    auto it = cache.find(name);
    if (it == cache.end())
    {
        uint8_t *cv = nullptr;
        for (const char *module : {"client.dll", "engine.dll", "materialsystem.dll"})
            if ((cv = FindConVar(module, name)) != nullptr)
                break;
        if (cv)
        {
            int &flags = *reinterpret_cast<int *>(cv + 20);
            flags &= ~(FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY);
            uint8_t *parent = *reinterpret_cast<uint8_t **>(cv + 28);
            if (parent && parent != cv && IsReadable(parent, 40))
                cv = parent;
            const char *cur = IsReadable(cv + 36, 4) ? *reinterpret_cast<const char **>(cv + 36) : nullptr;
            IVR_LOG("ConVar %s was \"%s\"", name, cur && IsReadable(cur, 1) ? cur : "?");
        }
        else
            IVR_LOG("ConVar %s not found (setting it anyway)", name);
        cache[name] = cv;
    }
    std::string cmd = std::string(name) + " " + value;
    ExecuteCommand(cmd.c_str());
}

void Game::ApplyVRSettings() const
{
    IVR_LOG("ConVar mat_queue_mode was \"%s\"", ConVarString("mat_queue_mode").c_str());
    // mat_queue_mode 0 is queued by the caller (console commands only run at a safe point);
    // [vr] multicore switches to 2 once the eye images exist.
    queueMode = 0;
    bool postOk = world::Active() || g_Config.renderFitWindow;
    if (g_Config.postProcessing && !postOk)
        IVR_LOG("Post-processing stays off: the engine's buffers are not at the eye size (world_at_eye_size)");
    else if (g_Config.postProcessing)
        IVR_LOG("Post-processing on (%d engine buffers at the eye size)", world::ResizedTargets());
    if (!g_Config.postProcessing || !postOk)
    {
        for (const char *cv : {"mat_postprocess_enable", "mat_dof_enabled", "mat_vignette_enable",
                               "mat_local_contrast_enable", "mat_motion_blur_enabled"})
            ForceConVar(cv, "0");
    }
    // Render the 3D view at full size straight into the eye image. A scaled-down 3D view is drawn
    // into a window-sized buffer and stretched up afterwards; on the larger eye images only its
    // top-left part holds the picture and the rest smears into lines.
    ForceConVar("mat_viewportscale", "1");
}

void *Game::CreateRenderTarget(const char *name, int w, int h, int format, int depthMode)
{
    bool *flag = matGameStartedFlag ? reinterpret_cast<bool *>(static_cast<uint8_t *>(materials) + matGameStartedFlag)
                                    : nullptr;
    bool saved = flag ? *flag : false;
    if (flag)
        *flag = false;
    VCall<void>(materials, vidx::BeginRenderTargetAllocation);
    if (flag)
        *flag = saved;
    void *tex = VCall<void *, const char *, int, int, int, int, int, unsigned, unsigned>(
        materials, vidx::CreateNamedRenderTargetTextureEx, name, w, h, (int)RT_SIZE_NO_CHANGE, format, depthMode,
        (unsigned)(TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT), 0u);
    // EndRenderTargetAllocation is a no-op while the game-started flag is set; the
    // allocation counter staying at 1 is harmless (it only gates this function).
    // Hold a reference: on a level load the material system frees every texture nobody
    // references, and ours would go with it (the headset then froze on the last frame).
    if (tex)
        VCall<void>(tex, vidx::TEX_IncrementReferenceCount);
    IVR_LOG("Created render target %s %dx%d -> %p", name, w, h, tex);
    return tex;
}

// INFRA's screen clicker is a static object (g_INFRAScreenClicker) whose first member is its vtable.
void Game::FindScreenClicker()
{
    void **vt = FindVTable("client.dll", ".?AVCINFRAScreenClicker@@");
    ModuleInfo mod = GetModuleInfo("client.dll");
    if (!vt || !mod.base)
    {
        IVR_LOG("INFRA screen clicker not found - its controller cursor stays as it is");
        return;
    }
    const uint32_t want = reinterpret_cast<uint32_t>(vt);
    uint8_t *known = mod.base + 0x5f66d4; // INFRA 2023 build
    if (IsReadable(known, 8) && *reinterpret_cast<uint32_t *>(known) == want)
        screenClicker = known;
    for (size_t off = 0; !screenClicker && off + 8 <= mod.size; off += 4)
    {
        uint8_t *p = mod.base + off;
        if (*reinterpret_cast<uint32_t *>(p) == want && reinterpret_cast<void **>(p) != vt - 1)
            screenClicker = p;
    }
    IVR_LOG("INFRA screen clicker at %p", screenClicker);
}

const uint8_t *Game::ScreenClickerPanel() const
{
    if (!screenClicker || !IsReadable(static_cast<uint8_t *>(screenClicker) + 4, 4))
        return nullptr;
    const uint8_t *panel = *reinterpret_cast<uint8_t *const *>(static_cast<uint8_t *>(screenClicker) + 4);
    return panel && IsReadable(panel, 0x1a0) ? panel : nullptr;
}

bool Game::StopControllerCursor() const
{
    const uint8_t *panel = ScreenClickerPanel();
    // +0x18B: the controller moves the cursor; +0x194..0x197: d-pad / stick directions held.
    if (!panel || (!panel[0x18B] && !*reinterpret_cast<const uint32_t *>(panel + 0x194)))
        return false;
    VCall<void, bool>(screenClicker, vidx::SetControllerMovesCursor, false); // also clears those
    return true;
}

} // namespace ivr

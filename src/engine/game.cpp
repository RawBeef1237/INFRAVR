#include "game.h"
#include "world_size.h"
#include "hooks.h"
#include "config.h"

#include <algorithm>
#include <cstring>
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

// CGrabController::SetTargetPosition(const Vector &, const QAngle &, bool teleport): stores
// m_shadow.targetPosition (+0x04) and targetRotation (+0x10). Not virtual; called at the end of
// UpdateObject / UpdateObjectVM with the hold point the game worked out.
const FnSpec kGrabSetTarget = {"CGrabController::SetTargetPosition", "server.dll", 0x3a7610,
                               "55 8B EC 80 7D 10 00 8B 45 0C 56 8B F1 57 8B 7D 08 D9 07 D9 5E 04 D9 47 04 D9 5E 08 "
                               "D9 47 08 D9 5E 0C D9 00 D9 5E 10"};

// CBaseEntity::SetCollisionGroup(int): "if (m_CollisionGroup (+0x148) != group) { ...; CollisionRulesChanged(); }".
const FnSpec kSetCollisionGroup = {"CBaseEntity::SetCollisionGroup", "server.dll", 0xcba40,
                                   "55 8B EC 53 56 57 8B 7D 08 8B D9 39 BB 48 01 00 00 74"};


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

// The target of a rel32 call (E8) at `at`, if there is one there.
void *CallTarget(const void *at)
{
    const uint8_t *p = static_cast<const uint8_t *>(at);
    if (!p || !IsReadable(p, 5) || p[0] != 0xE8)
        return nullptr;
    return const_cast<uint8_t *>(p + 5 + *reinterpret_cast<const int32_t *>(p + 1));
}

bool Game::Init()
{
    engineClient = Require("engine.dll", "VEngineClient015");
    renderView = Require("engine.dll", "VEngineRenderView013");
    entityList = Require("client.dll", "VClientEntityList003");
    materials = Require("materialsystem.dll", "VMaterialSystem080");
    surface = Require("vguimatsurface.dll", "VGUI_Surface031");
    vguiInput = Require("vgui2.dll", "VGUI_InputInternal001");
    vguiPanel = Require("vgui2.dll", "VGUI_Panel009");
    modelRender = Require("engine.dll", "VEngineModel016");
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
        // FindUseEntity (260) is what INFRA's selection outline asks every frame; it searches from
        // EyePosition (159) along EyeVectors, which reads EyeAngles (160).
        fnClientFindUseEntity = vt[260];
        fnClientEyePosition = vt[159];
        fnClientEyeAngles = vt[160];
        IVR_LOG("client: FindUseEntity %p, EyePosition %p, EyeAngles %p", fnClientFindUseEntity, fnClientEyePosition,
                fnClientEyeAngles);
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

bool Game::LightingAt(const Vector &origin, MaterialLightingState_t &out) const
{
    if (!modelRender)
        return false;
    LightingQuery_t q;
    q.origin = origin;
    void *envCubemap = nullptr; // ITexture*, written by the engine
    out = MaterialLightingState_t{};
    VCall<void, int, const LightingQuery_t *, MaterialLightingState_t *, void **>(
        modelRender, vidx::ComputeLightingState, 1, &q, &out, &envCubemap);
    if (out.localLightCount < 0 || out.localLightCount > 4)
        out.localLightCount = 0;
    return true;
}

Vector Game::ToneMapScale() const
{
    Vector s(1, 1, 1);
    void *ctx = materials ? RenderContext() : nullptr;
    if (!ctx)
        return s;
    // Vector GetToneMappingScaleLinear(): MSVC returns the struct through a hidden pointer.
    using Fn = Vector *(IVR_THISCALL *)(void *, Vector *);
    Vector r;
    reinterpret_cast<Fn>(VTable(ctx)[vidx::RC_GetToneMappingScaleLinear])(ctx, &r);
    ReleaseContext(ctx);
    if (r.x > 0.0f && r.x < 64.0f && r.y > 0.0f && r.y < 64.0f && r.z > 0.0f && r.z < 64.0f)
        s = r;
    return s;
}

std::string Game::MaterialTexture(const char *material, const char *var) const
{
    if (!materials)
        return {};
    void *mat = VCall<void *, const char *, const char *, bool, const char *>(materials, vidx::FindMaterial, material,
                                                                             nullptr, false, nullptr);
    if (!mat)
        return {};
    const char *name = VCall<const char *>(mat, vidx::MAT_GetName);
    if (!name || _stricmp(name, material) != 0) // the error material, or not what we asked for
        return {};
    bool found = false;
    void *mv = VCall<void *, const char *, bool *, bool>(mat, vidx::MAT_FindVar, var, &found, false);
    if (!mv || !found)
        return {};
    void *tex = VCall<void *>(mv, vidx::MV_GetTextureValue);
    const char *tn = tex ? VCall<const char *>(tex, vidx::TEX_GetName) : nullptr;
    return tn ? std::string(tn) : std::string();
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
        // FindUseEntity (439) searches from EyePosition (128, what Weapon_ShootPosition returns)
        // along EyeVectors, which reads EyeAngles (129).
        fnPlayerFindUseEntity = vt[439];
        fnPlayerEyePosition = vt[128];
    }
    else
    {
        fnPlayerEyeAngles = Resolve(kEyeAngles);
        fnPlayerShootPosition = Resolve(kShootPosition);
    }
    fnGrabSetTarget = Resolve(kGrabSetTarget);
    fnSetCollisionGroup = Resolve(kSetCollisionGroup);
    if (void **vt = FindVTable("server.dll", ".?AVCGrabController@@"))
        fnGrabDetach = vt[3];
    if (void **vt = FindVTable("server.dll", ".?AVCGrabController@@"))
        fnGrabAttach = vt[1];
    // CINFRA_Player::PickupObject (441): "movss xmm0, [size]" +0x19, "movss xmm0, [mass]" +0x2a,
    // "call CanPickupObject" +0x38, "call <already held>" +0x46.
    if (void **vt = FindVTable("server.dll", ".?AVCINFRA_Player@@"))
    {
        const uint8_t *p = static_cast<const uint8_t *>(vt[441]);
        if (p && IsReadable(p, 0x50) && memcmp(p + 0x19, "\xF3\x0F\x10\x05", 4) == 0 &&
            memcmp(p + 0x2a, "\xF3\x0F\x10\x05", 4) == 0 && p[0x38] == 0xE8 && p[0x46] == 0xE8)
        {
            const float *size = *reinterpret_cast<const float *const *>(p + 0x1d);
            const float *mass = *reinterpret_cast<const float *const *>(p + 0x2e);
            if (IsReadable(size, 4) && IsReadable(mass, 4))
            {
                pickupSize = *size;
                pickupMass = *mass;
                fnCanPickupObject = CallTarget(p + 0x38);
                fnIsHeldByPlayer = CallTarget(p + 0x46);
            }
        }
    }
    // CPlayerPickupController::Init (197) calls Pickup_OnPhysGunPickup at +0xc2, Shutdown (198)
    // Pickup_OnPhysGunDrop at +0xc5.
    if (void **vt = FindVTable("server.dll", ".?AVCPlayerPickupController@@"))
    {
        const uint8_t *init = static_cast<const uint8_t *>(vt[197]);
        const uint8_t *shut = static_cast<const uint8_t *>(vt[198]);
        if (init && IsReadable(init + 0xc2, 5) && init[0xc2] == 0xE8)
            fnOnPhysGunPickup = CallTarget(init + 0xc2);
        if (shut && IsReadable(shut + 0xc5, 5) && shut[0xc5] == 0xE8)
            fnOnPhysGunDrop = CallTarget(shut + 0xc5);
    }
    if (void **vt = FindVTable("server.dll", ".?AVCServerGameDLL@@"))
        fnGameFrame = vt[5];
    IVR_LOG("server: second hold: AttachEntity %p, CanPickupObject %p (mass %.0f, size %.0f), held check %p, "
            "OnPhysGunPickup %p, OnPhysGunDrop %p, GameFrame %p",
            fnGrabAttach, fnCanPickupObject, pickupMass, pickupSize, fnIsHeldByPlayer, fnOnPhysGunPickup,
            fnOnPhysGunDrop, fnGameFrame);
    // FindUseEntity checks each candidate with "push entity; mov ecx, edi; call CanUse" at +0x2e2;
    // the check starts with INFRA's aligned-stack prologue (53 8B DC).
    if (fnPlayerFindUseEntity)
    {
        void *t = CallTarget(static_cast<uint8_t *>(fnPlayerFindUseEntity) + 0x2e2);
        if (t && IsReadable(t, 3) && memcmp(t, "\x53\x8B\xDC", 3) == 0)
            fnPlayerCanUse = t;
    }
    // ComputeError reads player_held_object_collide_with_player with "mov eax, [cvar]" (A1) at +0x5db.
    if (fnGrabComputeError)
    {
        const uint8_t *p = static_cast<const uint8_t *>(fnGrabComputeError) + 0x5db;
        if (IsReadable(p, 5) && p[0] == 0xA1)
            heldCollideCvar = *reinterpret_cast<void *const *>(p + 1);
    }
    IVR_LOG("server: UpdateObject %p, UpdateObjectVM %p, ComputeError %p, EyeAngles %p, Weapon_ShootPosition %p",
            fnGrabUpdateObject, fnGrabUpdateObjectVM, fnGrabComputeError, fnPlayerEyeAngles, fnPlayerShootPosition);
    IVR_LOG("server: SetTargetPosition %p, FindUseEntity %p, EyePosition %p", fnGrabSetTarget, fnPlayerFindUseEntity,
            fnPlayerEyePosition);
    IVR_LOG("server: CanUse %p, SetCollisionGroup %p, DetachEntity %p, held-object collide cvar %p", fnPlayerCanUse,
            fnSetCollisionGroup, fnGrabDetach, heldCollideCvar);
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
    UnlockConVar(name);
    std::string cmd = std::string(name) + " " + value;
    ExecuteCommand(cmd.c_str());
}

void Game::UnlockConVar(const char *name) const
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

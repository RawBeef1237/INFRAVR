// INFRA VR - Source engine access for INFRA (Portal 2 / Alien Swarm engine branch).
//
// Every index/offset below was verified against INFRA's own binaries
// (engine.dll 2017-10-06, client.dll/server.dll 2017-12-18). They are NOT the Source
// SDK 2013 numbers - INFRA runs on the Portal 2 branch of the engine.
#pragma once
#include "common.h"
#include "mathlib.h"
#include "vfunc.h"

#include <string>

namespace ivr {

// --- CViewSetup (Alien Swarm / Portal 2 layout, 0xF4 bytes) ---------------------------
struct CViewSetup
{
    int x, y, width, height;          // 0x00
    bool m_bOrtho;                    // 0x10
    float orthoLeft, orthoTop, orthoRight, orthoBottom; // 0x14
    bool m_bCustomViewMatrix;         // 0x24
    float m_matCustomViewMatrix[3][4]; // 0x28
    float fov;                        // 0x58
    float fovViewmodel;               // 0x5C
    Vector origin;                    // 0x60
    QAngle angles;                    // 0x6C
    float zNear;                      // 0x78
    float zFar;                       // 0x7C
    float zNearViewmodel;             // 0x80
    float zFarViewmodel;              // 0x84
    float m_flAspectRatio;            // 0x88
    uint8_t rest[0xF4 - 0x8C];        // DoF, motion blur, off-center (0xE0..0xEC, flag byte 0xF0)
};
static_assert(sizeof(CViewSetup) == 0xF4, "CViewSetup size");
static_assert(offsetof(CViewSetup, fov) == 0x58, "fov");
static_assert(offsetof(CViewSetup, origin) == 0x60, "origin");
static_assert(offsetof(CViewSetup, m_flAspectRatio) == 0x88, "aspect");

// Off-center projection (Alien Swarm layout): the view draws only part of the frustum given by fov
// and aspect - top, bottom, left, right as 0..1 across it - when bit 0 of the flag byte is set.
inline float *OffCenterTBLR(CViewSetup &v) { return reinterpret_cast<float *>(&v.rest[0xE0 - 0x8C]); }
inline const float *OffCenterTBLR(const CViewSetup &v) { return reinterpret_cast<const float *>(&v.rest[0xE0 - 0x8C]); }
inline bool IsOffCenter(const CViewSetup &v) { return (v.rest[0xF0 - 0x8C] & 1u) != 0; }
inline void SetOffCenter(CViewSetup &v, bool on)
{
    if (on)
        v.rest[0xF0 - 0x8C] |= 1u;
    else
        v.rest[0xF0 - 0x8C] &= ~1u;
}

// --- CUserCmd ------------------------------------------------------------------------
struct CUserCmd
{
    void *vtable;       // 0x00
    int command_number; // 0x04
    int tick_count;     // 0x08
    QAngle viewangles;  // 0x0C
    float forwardmove;  // 0x18
    float sidemove;     // 0x1C
    float upmove;       // 0x20
    int buttons;        // 0x24
    uint8_t impulse;    // 0x28
};

enum : int
{
    IN_ATTACK = 1 << 0,
    IN_JUMP = 1 << 1,
    IN_DUCK = 1 << 2,
    IN_FORWARD = 1 << 3,
    IN_BACK = 1 << 4,
    IN_USE = 1 << 5,
    IN_MOVELEFT = 1 << 9,
    IN_MOVERIGHT = 1 << 10,
    IN_ATTACK2 = 1 << 11,
    IN_RELOAD = 1 << 13,
    IN_SPEED = 1 << 17,
    IN_ZOOM = 1 << 19,
};

enum : int
{
    RENDERVIEW_DRAWVIEWMODEL = 1 << 0,
    RENDERVIEW_DRAWHUD = 1 << 1,
    RENDERVIEW_SUPPRESSMONITORRENDERING = 1 << 2,
};

enum : int
{
    PAINT_UIPANELS = 1 << 0,
    PAINT_INGAMEPANELS = 1 << 1,
};

// Material system enums
enum : int
{
    IMAGE_FORMAT_BGRA8888 = 12,
    RT_SIZE_NO_CHANGE = 0,
    MATERIAL_RT_DEPTH_SEPARATE = 1,
    MATERIAL_RT_DEPTH_NONE = 2,
    TEXTUREFLAGS_CLAMPS = 0x4,
    TEXTUREFLAGS_CLAMPT = 0x8,
    TEXTUREFLAGS_NOMIP = 0x100,
    TEXTUREFLAGS_NOLOD = 0x200,
};

// --- vtable indices (verified in INFRA) ----------------------------------------------
namespace vidx {
// IVEngineClient (CEngineClient, "VEngineClient015")
constexpr int GetLocalPlayer = 12;
constexpr int GetViewAngles = 18;
constexpr int SetViewAngles = 19;
constexpr int IsInGame = 25;
constexpr int IsConnected = 26;
constexpr int ExecuteClientCmd = 104;
// IClientEntityList ("VClientEntityList003")
constexpr int GetClientEntity = 3;
constexpr int GetClientEntityFromHandle = 4;
// IMaterialSystem (CMaterialSystem, "VMaterialSystem080")
constexpr int GetBackBufferDimensions = 33;
constexpr int GetBackBufferFormat = 34;
constexpr int BeginRenderTargetAllocation = 91;
constexpr int EndRenderTargetAllocation = 92;
constexpr int CreateNamedRenderTargetTextureEx = 96;
constexpr int GetRenderContext = 112;
constexpr int FindMaterial = 81; // IMaterial* (const char *name, const char *group, bool complain, const char *prefix)
// IMaterial / IMaterialVar
constexpr int MAT_GetName = 0;
constexpr int MAT_FindVar = 11;         // IMaterialVar* (const char *name, bool *found, bool complain)
constexpr int MV_GetTextureValue = 0;   // ITexture*
// IMatRenderContext
constexpr int RC_Release = 1;
constexpr int RC_SetRenderTarget = 6;
constexpr int RC_ClearBuffers = 12;
constexpr int RC_ClearColor4ub = 74;
constexpr int RC_PushRenderTargetAndViewport = 107;
constexpr int RC_PopRenderTargetAndViewport = 111;
constexpr int RC_OverrideAlphaWriteEnable = 184;
constexpr int RC_GetCallQueue = 141; // ICallQueue* (null unless mat_queue_mode is queued)
// ITexture
constexpr int TEX_GetName = 0;
constexpr int TEX_GetActualWidth = 3;
constexpr int TEX_GetActualHeight = 4;
constexpr int TEX_IncrementReferenceCount = 10;
// IMatRenderContext::GetToneMappingScaleLinear: returns a Vector (MSVC: hidden result pointer, ret 4)
constexpr int RC_GetToneMappingScaleLinear = 145;
// IVModelRender (CModelRender, "VEngineModel016")
constexpr int ComputeLightingState = 26;
// IVRenderView ("VEngineRenderView013")
constexpr int VGui_Paint = 38;
// ISurface ("VGUI_Surface031")
constexpr int IsCursorVisible = 56;
constexpr int IsCursorLocked = 107;
// vgui::IInputInternal (vgui2.dll VGUI_InputInternal001, checked against CInputWin32's vtable)
constexpr int InternalKeyCodePressed = 74;
constexpr int InternalKeyCodeReleased = 77;
// C_BasePlayer
constexpr int GetActiveWeapon = 243;
// IINFRAScreenClicker (client.dll, the mouse/controller cursor on INFRA's in-world menu screens)
constexpr int SetControllerMovesCursor = 4;
} // namespace vidx

// World lighting at a point, as the engine lights a model there (IVModelRender::ComputeLightingState).
// Alien Swarm / Portal 2 layouts, checked in INFRA's engine.dll and engine.dylib (see OFFSETS.md).
struct LightingQuery_t
{
    Vector origin;                  // 0x00
    unsigned short instance = 0xFFFF; // 0x0C ModelInstanceHandle_t (none)
    bool ambientBoost = false;      // 0x0E
};
static_assert(sizeof(LightingQuery_t) == 0x10, "LightingQuery_t");

struct LightDesc_t
{
    int type;              // 0x00 0 off, 1 point, 2 directional, 3 spot
    Vector color;          // 0x04 linear
    Vector position;       // 0x10
    Vector direction;      // 0x1C the way the light travels
    float range;           // 0x28 0 = unlimited
    float falloff;         // 0x2C spot falloff exponent
    float attenuation0;    // 0x30 constant
    float attenuation1;    // 0x34 linear
    float attenuation2;    // 0x38 quadratic
    float theta, phi;      // 0x3C / 0x40 spot inner / outer angles
    float thetaDot, phiDot; // 0x44 / 0x48 their cosines
    float oneOverThetaDotMinusPhiDot; // 0x4C
    uint32_t flags;        // 0x50
    float rangeSquared;    // 0x54
};
static_assert(sizeof(LightDesc_t) == 0x58, "LightDesc_t");

struct MaterialLightingState_t
{
    Vector ambientCube[6]; // 0x00 +x -x +y -y +z -z, linear
    Vector lightingOrigin; // 0x48
    int localLightCount;   // 0x54
    LightDesc_t localLights[4]; // 0x58
};
static_assert(sizeof(MaterialLightingState_t) == 0x1B8, "MaterialLightingState_t");

struct Game
{
    // interfaces
    void *engineClient = nullptr;
    void *entityList = nullptr;
    void *materials = nullptr;
    void *renderView = nullptr;
    void *surface = nullptr;
    void *screenClicker = nullptr; // INFRA's IINFRAScreenClicker (main menu screen cursor)
    void *vguiInput = nullptr; // vgui::IInputInternal
    void *modelRender = nullptr; // IVModelRender ("VEngineModel016"), for world lighting
    size_t matGameStartedFlag = 0; // CMaterialSystem byte that blocks late RT allocation

    // functions (hook targets)
    void *fnRenderView = nullptr;          // CViewRender::RenderView
    void *fnRenderCameraWeapon = nullptr;  // CViewRender::DrawViewModels
    void *fnCreateMove = nullptr;          // ClientModeShared::CreateMove
    void *fnCalcViewModelView = nullptr;   // C_BasePlayer::CalcViewModelView
    void *fnFlashlightUpdate = nullptr;    // CFlashlightEffect::UpdateLight
    void *fnInfraFlashlightUpdate = nullptr; // INFRA's own flashlight: Update(Vector origin, Quaternion)
    void *fnBuildTransformations = nullptr; // C_BaseAnimating::BuildTransformations
    int boneArrayOffset = 0;               // C_BaseAnimating::m_BoneAccessor.m_pBones
    void *fnGrabUpdateObject = nullptr;    // CGrabController::UpdateObject (server)
    void *fnGrabUpdateObjectVM = nullptr;  // CGrabController::UpdateObjectVM (server)
    void *fnGrabComputeError = nullptr;    // CGrabController::ComputeError (server)
    void *fnPlayerEyeAngles = nullptr;     // CBasePlayer::EyeAngles (server)
    void *fnPlayerShootPosition = nullptr; // CBasePlayer::Weapon_ShootPosition (server)

    bool ready = false;

    // Multicore rendering (mat_queue_mode 2): the engine's own thread (CreateMove, RenderView)
    // and the thread that calls Present, which is the material system's render thread then.
    volatile DWORD mainThread = 0, presentThread = 0;
    mutable int queueMode = 0; // the mat_queue_mode we last asked for

    bool Init();
    void FindScreenClicker();
    bool InitServer(); // server.dll (singleplayer listen server, same process)

    // helpers ------------------------------------------------------------------------
    bool IsInGame() const { return engineClient && VCall<bool>(engineClient, vidx::IsInGame); }
    int LocalPlayerIndex() const { return engineClient ? VCall<int>(engineClient, vidx::GetLocalPlayer) : -1; }
    void *LocalPlayer() const;
    void *ActiveWeapon() const;
    bool IsCursorVisible() const { return surface && VCall<bool>(surface, vidx::IsCursorVisible); }
    bool IsCursorLocked() const { return surface && VCall<bool>(surface, vidx::IsCursorLocked); }
    // The screen clicker's panel (CINFRAScreenClickerPanel), or null before INFRA created it.
    const uint8_t *ScreenClickerPanel() const;
    // INFRA's main menu lets the controller move the mouse cursor once it is in controller mode;
    // that fights the VR pointer, so it is switched off while the pointer drives the menu.
    // Returns true if it was on.
    bool StopControllerCursor() const;
    // Presses and releases a ButtonCode_t (e.g. a gamepad button) straight into vgui, the way the
    // engine forwards INFRA's own controller to its menus. Main thread only. 
    // 1 = a panel took it, 0 = nothing has focus, -1 = no vgui input interface.
    int VguiButton(int code) const
    {
        if (!vguiInput)
            return -1;
        bool taken = VCall<bool, int>(vguiInput, vidx::InternalKeyCodePressed, code);
        VCall<void, int>(vguiInput, vidx::InternalKeyCodeReleased, code);
        return taken ? 1 : 0;
    }
    void SetViewAngles(const QAngle &a) const
    {
        QAngle copy = a;
        VCall<void, QAngle *>(engineClient, vidx::SetViewAngles, &copy);
    }
    void ExecuteCommand(const char *cmd) const;
    // Sets a console variable, first lifting its cheat/development-only flags (found by
    // scanning the game modules for the ConVar object; nothing is called).
    void ForceConVar(const char *name, const char *value) const;
    std::string ConVarString(const char *name) const; // current value, "" if not found
    // Console settings the stereo rendering needs (run at start and on every level load).
    void ApplyVRSettings() const;

    void *RenderContext() const { return VCall<void *>(materials, vidx::GetRenderContext); }
    // Main thread, during rendering: how the engine would light a model at `origin` (ambient cube
    // and up to 4 local lights). False if the interface is missing.
    bool LightingAt(const Vector &origin, MaterialLightingState_t &out) const;
    // HDR exposure the game's shaders multiply by (1 1 1 without HDR).
    Vector ToneMapScale() const;
    // The texture a material's texture variable points at now (e.g. "$basetexture"), "" if unknown.
    std::string MaterialTexture(const char *material, const char *var) const;
    static void ReleaseContext(void *ctx) { VCall<int>(ctx, vidx::RC_Release); }
    void BackBufferSize(int &w, int &h) const
    {
        VCall<void, int *, int *>(materials, vidx::GetBackBufferDimensions, &w, &h);
    }
    // Creates a render target after the game has started (normally refused by the engine).
    void *CreateRenderTarget(const char *name, int w, int h, int format, int depthMode);
};

extern Game g_Game;

} // namespace ivr

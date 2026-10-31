// INFRA VR - Source engine detours: stereo rendering, input, viewmodel, camera lens,
// flashlight and the server-side grab controller.
#include "config.h"
#include "glb_model.h"
#include "hands_renderer.h"
#include "world_size.h"
#include "d3d_vr.h"
#include "game.h"
#include "hooks.h"
#include "mod.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <functional>

namespace ivr {

namespace {

// ---- client ------------------------------------------------------------------------------
using RenderViewFn = void(IVR_FASTCALL *)(void *, void *, CViewSetup *, CViewSetup *, int, int);
using RenderCameraWeaponFn = void(IVR_FASTCALL *)(void *, void *, CViewSetup *, bool);
using CreateMoveFn = bool(IVR_FASTCALL *)(void *, void *, float, CUserCmd *);
using CalcViewModelViewFn = void(IVR_FASTCALL *)(void *, void *, const Vector *, const QAngle *);
using UpdateLightFn = void(IVR_FASTCALL *)(void *, void *, int, const Vector *, const Vector *, const Vector *,
                                           const Vector *, float, float, float, bool, const char *);
using InfraFlashlightFn = void(IVR_FASTCALL *)(void *, void *, float, float, float, float, float, float, float);
using BuildTransformationsFn = void(IVR_FASTCALL *)(void *, void *, void *, void *, void *, const void *, int, void *);
// ---- server ------------------------------------------------------------------------------
using UpdateObjectFn = bool(IVR_FASTCALL *)(void *, void *, void *, float, bool);
using UpdateObjectVMFn = bool(IVR_FASTCALL *)(void *, void *, void *, float);
using ComputeErrorFn = float(IVR_FASTCALL *)(void *, void *);
using EyeAnglesFn = const QAngle *(IVR_FASTCALL *)(void *, void *);
using ShootPositionFn = Vector *(IVR_FASTCALL *)(void *, void *, Vector *);

Hook<RenderViewFn> hkRenderView;
Hook<RenderCameraWeaponFn> hkRenderCameraWeapon;
Hook<CreateMoveFn> hkCreateMove;
Hook<CalcViewModelViewFn> hkCalcViewModelView;
Hook<UpdateLightFn> hkUpdateLight;
Hook<BuildTransformationsFn> hkBuildTransformations;
Hook<InfraFlashlightFn> hkInfraFlashlight;
Hook<UpdateObjectFn> hkUpdateObject;
Hook<UpdateObjectVMFn> hkUpdateObjectVM;
Hook<ComputeErrorFn> hkComputeError;
Hook<EyeAnglesFn> hkEyeAngles;
Hook<ShootPositionFn> hkShootPosition;

void *g_eyeRT[2] = {nullptr, nullptr};
void *g_hudRT = nullptr;
int g_hudW = 0, g_hudH = 0;
bool g_texturesFailed = false;
int g_renderDepth = 0;
int g_currentEye = -1;
CViewSetup g_engineView; // the engine's own (flat) view of this frame
int g_grabOverride = 0;
double g_lastLog = 0;
double g_traceAgainAt = 0;

// ---- multicore rendering ---------------------------------------------------------------------
// With mat_queue_mode 2 the engine records its rendering on this thread and plays it back on a
// render thread later, so anything we do at the D3D level (checking what the game drew into,
// the camera copy, telling Present a stereo frame is ready) must be queued the same way. A
// CFunctor laid out for MSVC: AddRef, Release, deleting destructor, operator().
struct QueuedCall
{
    void **vtable;
    unsigned userId = 0;
    volatile LONG refs = 0;
    std::function<void()> fn;
};

int IVR_FASTCALL QcAddRef(QueuedCall *self, void *) { return (int)InterlockedIncrement(&self->refs); }
int IVR_FASTCALL QcRelease(QueuedCall *self, void *)
{
    LONG n = InterlockedDecrement(&self->refs);
    if (n == 0)
        delete self;
    return (int)n;
}
void *IVR_FASTCALL QcDestroy(QueuedCall *self, void *, int flags)
{
    if (flags & 1)
        delete self;
    return self;
}
void IVR_FASTCALL QcCall(QueuedCall *self, void *)
{
    if (self->fn)
        self->fn();
}
void *g_qcVtable[4] = {(void *)&QcAddRef, (void *)&QcRelease, (void *)&QcDestroy, (void *)&QcCall};

void *RenderQueue()
{
    void *ctx = g_Game.RenderContext();
    void *queue = ctx ? VCall<void *>(ctx, vidx::RC_GetCallQueue) : nullptr;
    if (ctx)
        Game::ReleaseContext(ctx);
    return queue;
}

} // namespace

// Runs fn where the D3D work of what was rendered so far happens: now, or queued after it.
void Defer(std::function<void()> fn)
{
    void *queue = RenderQueue();
    if (!queue)
    {
        fn();
        return;
    }
    QueuedCall *c = new QueuedCall;
    c->vtable = g_qcVtable;
    c->fn = std::move(fn);
    c->refs = 1; // ICallQueue::QueueFunctor's AddRef; the queue releases it after the call
    VCall<void, void *>(queue, 0, c); // QueueFunctorInternal
}

namespace {

// mat_queue_mode 2 only while we are in a level with the eye images made (creating render
// targets and the menus keep the engine single threaded).
void UpdateQueueMode()
{
    int want = (g_Config.multicore && !g_Mod.menuMode && g_D3D.eye[0].Valid() && g_D3D.eye[1].Valid()) ? 2 : 0;
    if (want == g_Game.queueMode)
        return;
    g_Game.queueMode = want;
    g_Game.ExecuteCommand(want == 2 ? "mat_queue_mode 2" : "mat_queue_mode 0");
    IVR_LOG("Multicore rendering %s", want == 2 ? "on" : "off");
}

// Stereo once per Present (Black Mesa VR's rule): with mat_queue_mode 0 a second main view before
// the next Present (some map scripts and the engine's "screen" views do that) would draw both eyes
// again and the first pair would never be seen. Later views in the same frame are drawn flat.
volatile LONG g_stereoSincePresent = 0;
int g_stereoSkipped = 0;

} // namespace

void ResetStereoGuard() { InterlockedExchange(&g_stereoSincePresent, 0); }

namespace {

bool g_rtLost = false; // the game stopped drawing into our eye images: make new ones
int g_rtGeneration = 0;

bool EnsureTextures()
{
    if (g_rtLost)
    {
        g_rtLost = false;
        if (g_rtGeneration < 3)
        {
            ++g_rtGeneration;
            IVR_ERR("The game is drawing the eyes into its window instead of the eye images - creating new ones");
            g_eyeRT[0] = g_eyeRT[1] = g_hudRT = nullptr;
            g_D3D.eye[0].Reset();
            g_D3D.eye[1].Reset();
            g_D3D.hud.Reset();
        }
    }
    if (g_eyeRT[0] && g_eyeRT[1])
        return true;
    if (g_texturesFailed)
        return false;
    if (RenderQueue())
    {
        // Render targets are made with the engine single threaded; UpdateQueueMode turns
        // multicore off because the eye images are missing, so this works a frame later.
        g_D3D.eye[0].Reset();
        g_D3D.eye[1].Reset();
        return false;
    }
    int w = (int)g_VR.renderWidth, h = (int)g_VR.renderHeight;
    if (g_Config.renderFitWindow)
    {
        int bw = 0, bh = 0;
        g_Game.BackBufferSize(bw, bh);
        float s = std::min(1.0f, std::min(bw / (float)w, bh / (float)h));
        w = (int)(w * s) & ~1;
        h = (int)(h * s) & ~1;
        g_VR.renderWidth = (uint32_t)w;
        g_VR.renderHeight = (uint32_t)h;
        IVR_LOG("render_fit_window: eye images %dx%d (window %dx%d)", w, h, bw, bh);
    }
    int format = VCall<int>(g_Game.materials, vidx::GetBackBufferFormat);
    for (int e = 0; e < 2; ++e)
    {
        g_D3D.capturing = e;
        char name[48];
        snprintf(name, sizeof(name), "_rt_infravr_%s%s", e == 0 ? "left" : "right",
                 g_rtGeneration ? std::to_string(g_rtGeneration).c_str() : "");
        g_eyeRT[e] = g_Game.CreateRenderTarget(name, w, h, format, MATERIAL_RT_DEPTH_SEPARATE);
        g_D3D.capturing = -1;
    }
    g_Game.BackBufferSize(g_hudW, g_hudH);
    if (g_hudW > 0 && g_hudH > 0) // also needed for the phone menu panel in any hud_mode
    {
        g_D3D.capturing = 2;
        std::string name = "_rt_infravr_hud" + (g_rtGeneration ? std::to_string(g_rtGeneration) : std::string());
        g_hudRT = g_Game.CreateRenderTarget(name.c_str(), g_hudW, g_hudH, IMAGE_FORMAT_BGRA8888,
                                            MATERIAL_RT_DEPTH_NONE);
        g_D3D.capturing = -1;
    }
    if (!g_eyeRT[0] || !g_eyeRT[1])
    {
        IVR_ERR("Could not create the eye render targets - VR rendering disabled");
        g_texturesFailed = true;
        return false;
    }
    IVR_LOG("Eye render targets %dx%d (format %d): D3D textures %s/%s, HUD %s", w, h, format,
            g_D3D.eye[0].Valid() ? "captured" : "pending", g_D3D.eye[1].Valid() ? "captured" : "pending",
            g_D3D.hud.Valid() ? "captured" : (g_hudRT ? "pending" : "off"));
    return true;
}

bool IsMainView(const CViewSetup *view)
{
    int bw = 0, bh = 0;
    g_Game.BackBufferSize(bw, bh);
    return view->x == 0 && view->y == 0 && view->width >= bw * 9 / 10 && view->height >= bh * 9 / 10;
}

void SetRenderTarget(void *tex)
{
    void *ctx = g_Game.RenderContext();
    VCall<void, void *>(ctx, vidx::RC_SetRenderTarget, tex);
    Game::ReleaseContext(ctx);
}

void PaintHud(const CViewSetup *hudView, int paintMode)
{
    void *ctx = g_Game.RenderContext();
    CViewSetup hv = *hudView;
    hv.x = hv.y = 0;
    hv.width = g_hudW;
    hv.height = g_hudH;
    alignas(16) uint8_t frustum[512] = {};
    // IVRenderView::Push2DView(IMatRenderContext*, const CViewSetup&, int flags, ITexture*, Frustum)
    VCall<void, void *, CViewSetup *, int, void *, void *>(g_Game.renderView, 41, ctx, &hv, 0, g_hudRT, frustum);
    VCall<void, bool, bool>(ctx, vidx::RC_OverrideAlphaWriteEnable, true, true);
    VCall<void, unsigned char, unsigned char, unsigned char, unsigned char>(ctx, vidx::RC_ClearColor4ub, 0, 0, 0, 0);
    VCall<void, bool, bool, bool>(ctx, vidx::RC_ClearBuffers, true, false, false);
    int hw = g_hudW, hh = g_hudH;
    Defer([hw, hh] {
        if (!g_D3D.hud.Valid())
            g_D3D.CaptureBoundRenderTarget(g_D3D.hud, (UINT)hw, (UINT)hh);
        else
            g_D3D.CheckBoundRenderTarget(2);
    });
    VCall<void, int>(g_Game.renderView, vidx::VGui_Paint, paintMode);
    VCall<void, bool, bool>(ctx, vidx::RC_OverrideAlphaWriteEnable, false, true);
    VCall<void, void *, void *>(g_Game.renderView, 42, ctx, frustum); // PopView(ctx, frustum)
    Game::ReleaseContext(ctx);
}

bool g_lensPass = false;

// The camera weapon's screen shows _rt_CameraWeapon, which DrawViewModels fills with a copy of the
// frame drawn so far (rect = the view's size). The eye views are bigger than that texture, so the
// copy failed and the screen kept an old picture (the main menu). Draw one extra, texture-sized
// view from the camera lens in your hand; its copy fills the screen, and the eye views are kept
// from overwriting it.
void RenderLensView(void *self, void *edx, CViewSetup *view, CViewSetup *hudView, int clearFlags, int whatToDraw)
{
    int bw = 0, bh = 0;
    g_Game.BackBufferSize(bw, bh);
    int w = std::min(bw, (int)g_VR.renderWidth), h = std::min(bh, (int)g_VR.renderHeight);
    if (w <= 0 || h <= 0)
        return;
    CViewSetup v = *view;
    v.x = v.y = 0;
    v.width = w;
    v.height = h;
    v.m_flAspectRatio = (float)w / (float)h;
    v.zNear = g_Config.zNear;
    v.m_bOrtho = false;
    v.m_bCustomViewMatrix = false;
    v.rest[0xF0 - 0x8C] &= ~1u; // m_bOffCenter
    Pose lens = g_Mod.LensWorld();
    v.origin = lens.pos;
    v.angles = lens.Angles();
    CViewSetup hv = *hudView;
    hv.x = hv.y = 0;
    hv.width = w;
    hv.height = h;
    SetRenderTarget(g_eyeRT[0]); // the left eye redraws all of it straight after
    g_lensPass = true;
    hkRenderView.original(self, edx, &v, &hv, clearFlags,
                          (whatToDraw & ~(RENDERVIEW_DRAWHUD | RENDERVIEW_DRAWVIEWMODEL)) |
                              RENDERVIEW_SUPPRESSMONITORRENDERING);
    g_lensPass = false;
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        IVR_LOG("Camera screen: drawing the lens view %dx%d, fov %.1f, every %d frame(s)", w, h, v.fov,
                g_Config.cameraScreenEvery);
    }
}

void IVR_FASTCALL RenderViewDetour(void *self, void *edx, CViewSetup *view, CViewSetup *hudView, int clearFlags,
                                   int whatToDraw)
{
    if (g_renderDepth == 0 && g_Mod.Active())
    {
        g_Game.mainThread = GetCurrentThreadId();
        // Present runs on the render thread (multicore): poses and input are read here instead.
        if (g_Game.presentThread && g_Game.presentThread != g_Game.mainThread)
            g_Mod.OnFrame();
        g_Mod.OnSafePoint(); // queued console commands
        UpdateQueueMode();
    }
    if (g_renderDepth > 0 || !g_Mod.Active() || g_Mod.menuMode || !IsMainView(view) || !EnsureTextures())
        return hkRenderView.original(self, edx, view, hudView, clearFlags, whatToDraw);
    if (g_Game.queueMode == 0 && g_stereoSincePresent > 0)
    {
        if (g_stereoSkipped++ < 5)
            IVR_LOG("A second main view before Present - drawn flat (stereo once per Present)");
        return hkRenderView.original(self, edx, view, hudView, clearFlags, whatToDraw);
    }
    InterlockedIncrement(&g_stereoSincePresent);

    ++g_renderDepth;
    g_engineView = *view;
    g_Mod.OnRenderViewBegin(*view);

    int w = (int)g_VR.ViewW(), h = (int)g_VR.ViewH(); // auto_fit may shrink this below the texture size
    CViewSetup hudEye = *hudView;
    hudEye.x = hudEye.y = 0;
    hudEye.width = w;
    hudEye.height = h;
    int flags = whatToDraw;
    if (g_Config.hudMode != HudMode::Eyes)
        flags &= ~RENDERVIEW_DRAWHUD;

    bool lensScreen = g_Config.cameraScreenView && g_Mod.activeItem == Item::Camera && !g_Mod.phoneMenu &&
                      g_Mod.HandValid(g_Mod.itemHand) && g_D3D.eye[0].Valid();
    static unsigned s_lensFrame = 0;
    if (lensScreen && (s_lensFrame++ % (unsigned)g_Config.cameraScreenEvery) == 0)
        RenderLensView(self, edx, view, hudView, clearFlags, whatToDraw);
    bool block = lensScreen && g_D3D.cameraSurface;
    Defer([block] { g_D3D.blockCameraCopy = block; });

    for (int eye = 0; eye < 2; ++eye)
    {
        CViewSetup v = *view;
        v.x = v.y = 0;
        v.width = w;
        v.height = h;
        v.fov = g_VR.fovX;
        v.fovViewmodel = g_VR.fovX;
        v.m_flAspectRatio = g_VR.aspect;
        v.zNear = g_Config.zNear;
        v.zNearViewmodel = 1.0f;
        v.m_bOrtho = false;
        v.m_bCustomViewMatrix = false;
        SetOffCenter(v, g_VR.offCenter);
        if (g_VR.offCenter)
        {
            float *tblr = OffCenterTBLR(v);
            g_VR.EyeOffCenter(eye, tblr[2], tblr[3], tblr[1], tblr[0]);
        }
        Pose p = g_Mod.EyeWorld(eye);
        v.origin = p.pos;
        v.angles = p.Angles();

        SetRenderTarget(g_eyeRT[eye]);
        g_currentEye = eye;
        static int s_traced = 0; // trace what the game does while drawing the left eye, twice
        bool trace = eye == 0 && s_traced < 2 && g_D3D.eye[0].Valid() && (s_traced == 0 || NowSeconds() > g_traceAgainAt);
        if (trace)
        {
            ++s_traced;
            g_traceAgainAt = NowSeconds() + 20.0;
            int tw = v.width, th = v.height;
            float tf = v.fov;
            Defer([tw, th, tf] {
                IVR_LOG("trace: left eye render begins (view %dx%d, fov %.1f)", tw, th, tf);
                g_D3D.trace = true;
                g_D3D.traceDraws = 0;
            });
        }
        world::BeginEye(); // post-processing works out its rectangles for the eye, not the window
        hkRenderView.original(self, edx, &v, &hudEye, clearFlags,
                              flags | (eye == 1 ? RENDERVIEW_SUPPRESSMONITORRENDERING : 0));
        world::EndEye();
        hands::DrawEye(eye, v);
        Defer([trace, eye] {
            if (trace)
            {
                if (g_D3D.traceDraws > 0)
                    IVR_LOG("  trace:   ... %d draws", g_D3D.traceDraws);
                g_D3D.trace = false;
                IVR_LOG("trace: left eye render ends");
            }
            if (g_D3D.BackBufferBound())
                g_rtLost = true; // picked up by EnsureTextures on the next frame
            else if (!g_D3D.eye[eye].Valid())
                g_D3D.CaptureBoundRenderTarget(g_D3D.eye[eye], g_VR.renderWidth, g_VR.renderHeight);
            else
                g_D3D.CheckBoundRenderTarget(eye);
        });
    }
    g_currentEye = -1;
    Defer([] { g_D3D.blockCameraCopy = false; });
    SetRenderTarget(nullptr);

    bool hud = false;
    if (g_hudRT && g_Mod.phoneMenu)
    {
        PaintHud(hudView, PAINT_UIPANELS | PAINT_INGAMEPANELS); // the pause menu, for the phone panel
        hud = true;
    }
    else if (g_Config.hudMode == HudMode::Overlay && g_hudRT && (whatToDraw & RENDERVIEW_DRAWHUD))
    {
        PaintHud(hudView, PAINT_INGAMEPANELS);
        hud = true;
    }
    vr::HmdMatrix34_t pose = g_VR.Head().raw; // what this frame was drawn for
    Defer([hud, pose] {
        g_D3D.SetRenderPose(pose);
        world::OnStereoFrame();
        g_Mod.eyesRendered = true;
        g_Mod.hudPainted = hud;
    });
    --g_renderDepth;

    double now = NowSeconds();
    static int s_frameLogs = 0; // every 5 s for the first minute, then every 30 s
    if (now - g_lastLog > (s_frameLogs < 12 ? 5.0 : 30.0))
    {
        g_lastLog = now;
        ++s_frameLogs;
        Pose head = g_Mod.HeadWorld();
        IVR_LOG("stereo frame: player eye (%.0f %.0f %.0f), head (%.0f %.0f %.0f), item %d", view->origin.x,
                view->origin.y, view->origin.z, head.pos.x, head.pos.y, head.pos.z, (int)g_Mod.activeItem);
    }
}

// CViewRender::DrawViewModels(const CViewSetup &view, bool drawViewModel): draws the first-person
// models into the current render target, using view.width/height and view.fovViewmodel. Each eye
// must get its own eye view (3.10 and earlier passed the flat 1920x1080 view once, which put the
// arms in the top-left corner of the left eye only).
void IVR_FASTCALL RenderCameraWeaponDetour(void *self, void *edx, CViewSetup *view, bool drawViewModel)
{
    static bool s_logged = false;
    if (g_renderDepth > 0 && !s_logged && view)
    {
        s_logged = true;
        IVR_LOG("Viewmodels drawn with view %dx%d, fov %.1f, viewmodel fov %.1f", view->width, view->height,
                view->fov, view->fovViewmodel);
    }
    if (g_lensPass)
    {
        LONG lw = view->width, lh = view->height;
        Defer([lw, lh] {
            g_D3D.learnCameraCopy = true; // its view-sized copy goes into the camera screen texture
            g_D3D.learnW = lw;
            g_D3D.learnH = lh;
        });
        hkRenderCameraWeapon.original(self, edx, view, drawViewModel);
        Defer([] { g_D3D.learnCameraCopy = false; });
        return;
    }
    hkRenderCameraWeapon.original(self, edx, view, drawViewModel);
}

bool IVR_FASTCALL CreateMoveDetour(void *self, void *edx, float sampleTime, CUserCmd *cmd)
{
    bool result = hkCreateMove.original(self, edx, sampleTime, cmd);
    if (!g_Mod.Active() || !cmd || !cmd->command_number)
        return result;
    g_Mod.OnCreateMove(cmd);
    return g_Mod.menuMode ? result : false; // we set the engine's view angles ourselves
}

// ---- tracked hands: the viewmodel's second arm follows the other controller -----------------
// Written in CalcViewModelView (main thread), read by the bone setup (possibly a worker thread).
struct HandTargets
{
    bool valid = false;
    Pose item, other; // controller poses in the viewmodel's world space
};
HandTargets g_handTargets;
CRITICAL_SECTION g_handLock;

void SetHandTargets(const HandTargets &t)
{
    EnterCriticalSection(&g_handLock);
    g_handTargets = t;
    LeaveCriticalSection(&g_handLock);
}

// One arm of INFRA's viewmodel rig (3ds Max biped: "Bip001 R Hand", ...).
struct Arm
{
    int clavicle = -1, upperArm = -1, forearm = -1, hand = -1;
};

const int kStudioBoneSize = 216; // mstudiobone_t (MDL v49)

int FindBone(const uint8_t *studio, const char *name)
{
    int count = *reinterpret_cast<const int *>(studio + 156);
    int index = *reinterpret_cast<const int *>(studio + 160);
    for (int i = 0; i < count; ++i)
    {
        const uint8_t *b = studio + index + i * kStudioBoneSize;
        if (std::strcmp(reinterpret_cast<const char *>(b + *reinterpret_cast<const int *>(b)), name) == 0)
            return i;
    }
    return -1;
}

int BoneParent(const uint8_t *studio, int bone)
{
    int index = *reinterpret_cast<const int *>(studio + 160);
    return *reinterpret_cast<const int *>(studio + index + bone * kStudioBoneSize + 4);
}

Arm FindArm(const uint8_t *studio, char side)
{
    char n[64];
    Arm a;
    std::snprintf(n, sizeof(n), "Bip001 %c Clavicle", side);
    a.clavicle = FindBone(studio, n);
    std::snprintf(n, sizeof(n), "Bip001 %c UpperArm", side);
    a.upperArm = FindBone(studio, n);
    std::snprintf(n, sizeof(n), "Bip001 %c Forearm", side);
    a.forearm = FindBone(studio, n);
    std::snprintf(n, sizeof(n), "Bip001 %c Hand", side);
    a.hand = FindBone(studio, n);
    return a;
}

// What the last viewmodel holds, read by CalcViewModelView on the next frame: which model arm
// carries the item and whether that item is the flashlight (v_light) rather than camera/phone.
volatile int g_vmItemSide = 1; // 0 = model's left arm, 1 = right
volatile bool g_vmItemIsLight = false;
const char *g_handsLoggedFor = nullptr;

// Side (0 = L, 1 = R) whose hand carries `bone`, or -1.
int SideOfBone(const uint8_t *studio, const Arm arm[2], const char *bone)
{
    int b = FindBone(studio, bone);
    for (int p = b; p >= 0; p = BoneParent(studio, p))
        for (int s = 0; s < 2; ++s)
            if (p == arm[s].hand)
                return s;
    return -1;
}

bool InSubtree(const uint8_t *studio, int bone, int root)
{
    for (int p = bone; p >= 0; p = BoneParent(studio, p))
        if (p == root)
            return true;
    return false;
}

void MoveOtherArm(void *self, void *studioHdr)
{
    HandTargets t;
    EnterCriticalSection(&g_handLock);
    t = g_handTargets;
    LeaveCriticalSection(&g_handLock);
    if (!t.valid || !studioHdr || !IsReadable(studioHdr, 4))
        return;
    const uint8_t *studio = *static_cast<const uint8_t *const *>(studioHdr);
    if (!studio || !IsReadable(studio, 0xA4))
        return;
    const char *name = reinterpret_cast<const char *>(studio + 12);
    if (!std::strstr(name, "weapons/v_") && !std::strstr(name, "weapons\\v_"))
        return;
    int count = *reinterpret_cast<const int *>(studio + 156);
    matrix3x4_t *bones = *reinterpret_cast<matrix3x4_t **>(static_cast<uint8_t *>(self) + g_Game.boneArrayOffset);
    if (count <= 0 || count > 256 || !bones || !IsReadable(bones, count * sizeof(matrix3x4_t)))
        return;

    Arm arm[2] = {FindArm(studio, 'L'), FindArm(studio, 'R')};
    if (arm[0].hand < 0 || arm[1].hand < 0 || arm[0].forearm < 0 || arm[1].forearm < 0)
        return;

    // The arm holding the item is already where CalcViewModelView put it (at that item's
    // controller); the other arm is moved onto the other controller.
    int side = SideOfBone(studio, arm, "Camera");
    if (side < 0)
        side = SideOfBone(studio, arm, "cellular_phone");
    bool light = false;
    if (side < 0 && (side = SideOfBone(studio, arm, "flashlight")) >= 0)
        light = true;
    if (side < 0)
        side = 1;
    g_vmItemSide = side;
    g_vmItemIsLight = light;
    const Arm &a = arm[side], &o = arm[1 - side];

    // Hand relative to its controller, mirrored onto the other controller. Fc flips the controller's
    // left axis; Fb (= diag(1, 1, -1)) maps a right biped hand bone frame to the left one and back,
    // measured from the bind poses of v_camera / v_phone / v_light.
    Pose g = t.item.Inverse() * bones[a.hand].ToPose();
    g.pos.y = -g.pos.y;
    for (int c = 0; c < 3; ++c)
        g.rot.m[1][c] = -g.rot.m[1][c];
    for (int r = 0; r < 3; ++r)
        g.rot.m[r][2] = -g.rot.m[r][2];
    Pose delta = (t.other * g) * bones[o.hand].ToPose().Inverse();

    for (int b = 0; b < count; ++b)
        if (InSubtree(studio, b, o.forearm))
            bones[b] = matrix3x4_t::FromPose(delta * bones[b].ToPose());
    if (g_Config.handsHideUpperArm)
    {
        // Everything above the forearms (shoulders, upper arms, spine) would float around the
        // controllers: zero rotation/scale collapses every vertex skinned to such a bone onto
        // that side's elbow.
        for (int b = 0; b < count; ++b)
        {
            if (InSubtree(studio, b, a.forearm) || InSubtree(studio, b, o.forearm))
                continue;
            const Arm &elbowArm = (b == o.clavicle || b == o.upperArm) ? o : a;
            Vector elbow = bones[elbowArm.forearm].ToPose().pos;
            for (int r = 0; r < 3; ++r)
            {
                bones[b].m[r][0] = bones[b].m[r][1] = bones[b].m[r][2] = 0.0f;
                bones[b].m[r][3] = elbow[r];
            }
        }
    }
    if (g_handsLoggedFor != (const char *)studio)
    {
        g_handsLoggedFor = (const char *)studio;
        IVR_LOG("Tracked hands: %s, %d bones, item (%s) in the model's %s hand", name, count,
                light ? "flashlight" : "camera/phone", side ? "right" : "left");
    }
}

// [camera] model: while INFRA draws v_camera, its animated bones drive the mod's camera model
// (same bone names). Returns true if this is v_camera and the bones were handed over.
bool CaptureCameraBones(const uint8_t *studio, const matrix3x4_t *bones, int count)
{
    const char *name = reinterpret_cast<const char *>(studio + 12);
    if (!std::strstr(name, "v_camera"))
        return false;
    const std::vector<std::string> &joints = hands::CameraJointNames();
    static const void *s_studio = nullptr;
    static std::vector<int> s_map;
    if (s_studio != studio)
    {
        s_studio = studio;
        s_map.assign(joints.size(), -1);
        int missing = 0;
        for (size_t j = 0; j < joints.size(); ++j)
            missing += (s_map[j] = FindBone(studio, joints[j].c_str())) < 0;
        IVR_LOG("Camera model: %s has %d of the model's %u bones%s", name, (int)joints.size() - missing,
                (unsigned)joints.size(), missing ? " - the others follow its hand" : "");
    }
    int hand = FindBone(studio, "Bip001 R Hand");
    std::vector<GlbMat34> world(joints.size());
    for (size_t j = 0; j < joints.size(); ++j)
    {
        int b = s_map[j] >= 0 ? s_map[j] : hand;
        if (b < 0 || b >= count)
            return false;
        std::memcpy(world[j].m, bones[b].m, sizeof(world[j].m));
    }
    hands::SetCameraBones(world);
    return true;
}

// [hands] model = glb: the mod draws its own hands, so INFRA's arms go - every bone outside the held
// item (camera, phone, flashlight) collapses to a point, which hides the vertices skinned to it.
// With the mod's camera model, v_camera's camera goes too (only its flashlight stays).
void HideArms(void *self, void *studioHdr)
{
    if (!studioHdr || !IsReadable(studioHdr, 4))
        return;
    const uint8_t *studio = *static_cast<const uint8_t *const *>(studioHdr);
    if (!studio || !IsReadable(studio, 0xA4))
        return;
    const char *name = reinterpret_cast<const char *>(studio + 12);
    if (!std::strstr(name, "weapons/v_") && !std::strstr(name, "weapons\\v_"))
        return;
    int count = *reinterpret_cast<const int *>(studio + 156);
    matrix3x4_t *bones = *reinterpret_cast<matrix3x4_t **>(static_cast<uint8_t *>(self) + g_Game.boneArrayOffset);
    if (count <= 0 || count > 256 || !bones || !IsReadable(bones, count * sizeof(matrix3x4_t)))
        return;
    bool cameraModel = hands::CameraModelEnabled() && CaptureCameraBones(studio, bones, count);
    int items[3] = {cameraModel ? -1 : FindBone(studio, "Camera"), FindBone(studio, "cellular_phone"),
                    FindBone(studio, "flashlight")};
    int hidden = 0;
    for (int b = 0; b < count; ++b)
    {
        bool keep = false;
        for (int it : items)
            keep = keep || (it >= 0 && InSubtree(studio, b, it));
        if (keep)
            continue;
        for (int r = 0; r < 3; ++r)
            bones[b].m[r][0] = bones[b].m[r][1] = bones[b].m[r][2] = 0.0f;
        ++hidden;
    }
    static const void *s_logged = nullptr;
    if (s_logged != studio)
    {
        s_logged = studio;
        IVR_LOG("GLB hands: %s - %d of %d bones hidden (INFRA's arms%s), item bones kept", name, hidden, count,
                cameraModel ? " and camera: the mod's camera model is drawn" : "");
    }
}

void IVR_FASTCALL BuildTransformationsDetour(void *self, void *edx, void *studioHdr, void *pos, void *q,
                                             const void *cameraTransform, int boneMask, void *computed)
{
    hkBuildTransformations.original(self, edx, studioHdr, pos, q, cameraTransform, boneMask, computed);
    if (g_Config.handsTracked || hands::Enabled())
        MoveOtherArm(self, studioHdr); // also finds which hand holds the item
    if (hands::Enabled())
        HideArms(self, studioHdr);
}

void IVR_FASTCALL CalcViewModelViewDetour(void *self, void *edx, const Vector *eyeOrigin, const QAngle *eyeAngles)
{
    if (!g_Mod.Active() || g_Mod.menuMode || !g_Mod.HandValid(g_Mod.itemHand))
    {
        SetHandTargets(HandTargets{});
        return hkCalcViewModelView.original(self, edx, eyeOrigin, eyeAngles);
    }
    CViewSetup anchor{};
    anchor.origin = *eyeOrigin;
    g_Mod.OnRenderViewBegin(anchor); // same eye position RenderView gets this frame
    // The flashlight model (no camera or phone) goes on the flashlight's hand.
    int anchorHand = g_vmItemIsLight && g_Mod.HandValid(g_Mod.flashlightHand)
                         ? g_Mod.flashlightHand
                         : g_Mod.itemHand;
    Pose hand = g_Mod.AimWorld(anchorHand);
    Vector off = g_Mod.activeItem == Item::Camera  ? g_Config.vmOffsetCamera
                 : g_Mod.activeItem == Item::Phone ? g_Config.vmOffsetPhone
                                                   : g_Config.vmOffsetDefault;
    QAngle extra(g_Config.vmAngles.x, g_Config.vmAngles.y, g_Config.vmAngles.z);
    if (g_Config.handsTracked && g_vmItemSide == 0)
    {
        // Offsets are tuned for an item in the model's right hand: mirror them for the left.
        off.y = -off.y;
        extra.y = -extra.y;
        extra.z = -extra.z;
    }
    Vector origin = hand.pos + hand.rot * off;
    QAngle angles = MatrixToAngles(hand.rot * AnglesToMatrix(extra));
    HandTargets targets;
    int other = 1 - anchorHand;
    if (g_Config.handsTracked && g_Mod.HandValid(other))
    {
        targets.valid = true;
        targets.item = hand;
        targets.other = g_Mod.AimWorld(other);
    }
    SetHandTargets(targets);
    hkCalcViewModelView.original(self, edx, &origin, &angles);
}

void IVR_FASTCALL UpdateLightDetour(void *self, void *edx, int entIndex, const Vector *pos, const Vector *forward,
                                    const Vector *right, const Vector *up, float fov, float farZ, float atten,
                                    bool shadows, const char *texture)
{
    // INFRA has one flashlight (the player's): its beam always comes from the flashlight hand,
    // whether or not the holster logic thinks the flashlight is out.
    bool hand = g_Mod.Active() && !g_Mod.menuMode && g_Mod.HandValid(g_Mod.flashlightHand);
    static int s_logs = 0;
    static double s_logAt = 0;
    if (s_logs < 6 && NowSeconds() >= s_logAt)
    {
        ++s_logs;
        s_logAt = NowSeconds() + 10.0;
        IVR_LOG("Flashlight update: entity %d (local player %d), %s", entIndex, g_Game.LocalPlayerIndex(),
                hand ? "moved to the flashlight hand" : "left where the game put it");
    }
    if (hand)
    {
        Pose p = g_Mod.AimWorld(g_Mod.flashlightHand);
        Vector P = p.pos + p.rot * g_Config.flashlightOffset;
        Vector F = p.rot.Forward(), R = -p.rot.Left(), U = p.rot.Up();
        return hkUpdateLight.original(self, edx, entIndex, &P, &F, &R, &U, fov, farZ, atten, shadows, texture);
    }
    hkUpdateLight.original(self, edx, entIndex, pos, forward, right, up, fov, farZ, atten, shadows, texture);
}

// INFRA's own flashlight (the one +flashlight turns on): the beam leaves the flashlight hand.
void IVR_FASTCALL InfraFlashlightDetour(void *self, void *edx, float ox, float oy, float oz, float qx, float qy,
                                       float qz, float qw)
{
    static bool s_logged = false;
    if (g_Mod.Active() && !g_Mod.menuMode && g_Mod.HandValid(g_Mod.flashlightHand))
    {
        Pose p = g_Mod.AimWorld(g_Mod.flashlightHand);
        Vector P = p.pos + p.rot * g_Config.flashlightOffset;
        Quat q = Mat3ToQuat(p.rot); // forward, left, up columns, as Source's BasisToQuaternion
        if (!s_logged)
        {
            s_logged = true;
            IVR_LOG("INFRA flashlight: moved from (%.0f %.0f %.0f) to the flashlight hand (%.0f %.0f %.0f)", ox, oy,
                    oz, P.x, P.y, P.z);
        }
        return hkInfraFlashlight.original(self, edx, P.x, P.y, P.z, q.x, q.y, q.z, q.w);
    }
    hkInfraFlashlight.original(self, edx, ox, oy, oz, qx, qy, qz, qw);
}

// ---- server: hold grabbed objects in front of the hand instead of the eyes -----------------
bool IVR_FASTCALL UpdateObjectDetour(void *self, void *edx, void *player, float error, bool teleport)
{
    g_Mod.lastGrabUpdate = NowSeconds();
    ++g_grabOverride;
    bool r = hkUpdateObject.original(self, edx, player, error, teleport);
    --g_grabOverride;
    return r;
}

bool IVR_FASTCALL UpdateObjectVMDetour(void *self, void *edx, void *player, float error)
{
    g_Mod.lastGrabUpdate = NowSeconds();
    ++g_grabOverride;
    bool r = hkUpdateObjectVM.original(self, edx, player, error);
    --g_grabOverride;
    return r;
}

float IVR_FASTCALL ComputeErrorDetour(void *self, void *edx)
{
    ++g_grabOverride;
    float r = hkComputeError.original(self, edx);
    --g_grabOverride;
    return r;
}

const QAngle *IVR_FASTCALL EyeAnglesDetour(void *self, void *edx)
{
    if (g_grabOverride > 0 && g_Mod.Active() && !g_Mod.menuMode)
        return &g_Mod.grabHandAngles;
    return hkEyeAngles.original(self, edx);
}

Vector *IVR_FASTCALL ShootPositionDetour(void *self, void *edx, Vector *out)
{
    Vector *r = hkShootPosition.original(self, edx, out);
    if (g_grabOverride > 0 && g_Mod.Active() && !g_Mod.menuMode && r)
        *r = g_Mod.grabHandPos;
    return r;
}

} // namespace

bool InstallClientHooks()
{
    bool ok = hkRenderView.Install("CViewRender::RenderView", g_Game.fnRenderView, (void *)&RenderViewDetour);
    ok &= hkCreateMove.Install("ClientModeShared::CreateMove", g_Game.fnCreateMove, (void *)&CreateMoveDetour);
    if (g_Game.fnRenderCameraWeapon)
        hkRenderCameraWeapon.Install("CViewRender::RenderCameraWeapon", g_Game.fnRenderCameraWeapon,
                                     (void *)&RenderCameraWeaponDetour);
    if (g_Game.fnCalcViewModelView)
        hkCalcViewModelView.Install("C_BasePlayer::CalcViewModelView", g_Game.fnCalcViewModelView,
                                    (void *)&CalcViewModelViewDetour);
    InitializeCriticalSection(&g_handLock);
    if (g_Game.fnBuildTransformations)
        hkBuildTransformations.Install("C_BaseAnimating::BuildTransformations", g_Game.fnBuildTransformations,
                                       (void *)&BuildTransformationsDetour);
    if (g_Game.fnInfraFlashlightUpdate)
        hkInfraFlashlight.Install("INFRA flashlight update", g_Game.fnInfraFlashlightUpdate,
                                  (void *)&InfraFlashlightDetour);
    if (g_Game.fnFlashlightUpdate)
        hkUpdateLight.Install("CFlashlightEffect::UpdateLight", g_Game.fnFlashlightUpdate,
                              (void *)&UpdateLightDetour);
    return ok;
}

bool InstallServerHooks()
{
    if (!g_Game.InitServer())
        return false;
    hkUpdateObject.Install("CGrabController::UpdateObject", g_Game.fnGrabUpdateObject, (void *)&UpdateObjectDetour);
    if (g_Game.fnGrabUpdateObjectVM)
        hkUpdateObjectVM.Install("CGrabController::UpdateObjectVM", g_Game.fnGrabUpdateObjectVM,
                                 (void *)&UpdateObjectVMDetour);
    if (g_Game.fnGrabComputeError)
        hkComputeError.Install("CGrabController::ComputeError", g_Game.fnGrabComputeError,
                               (void *)&ComputeErrorDetour);
    hkEyeAngles.Install("CBasePlayer::EyeAngles", g_Game.fnPlayerEyeAngles, (void *)&EyeAnglesDetour);
    hkShootPosition.Install("CBasePlayer::Weapon_ShootPosition", g_Game.fnPlayerShootPosition,
                            (void *)&ShootPositionDetour);
    return true;
}

} // namespace ivr

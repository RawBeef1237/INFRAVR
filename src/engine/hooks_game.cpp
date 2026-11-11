// INFRA VR - Source engine detours: stereo rendering, input, viewmodel, camera lens,
// flashlight and the server-side grab controller.
#include "config.h"
#include "glb_model.h"
#include "hands_renderer.h"
#include "flags.h"
#include "world_size.h"
#include "d3d_vr.h"
#include "game.h"
#include "hooks.h"
#include "mod.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <functional>

namespace ivr {
void Defer(std::function<void()> fn);

namespace {

// ---- client ------------------------------------------------------------------------------
using RenderViewFn = void(IVR_FASTCALL *)(void *, void *, CViewSetup *, CViewSetup *, int, int);
using RenderCameraWeaponFn = void(IVR_FASTCALL *)(void *, void *, CViewSetup *, bool);
using CreateMoveFn = bool(IVR_FASTCALL *)(void *, void *, float, CUserCmd *);
using CalcViewModelViewFn = void(IVR_FASTCALL *)(void *, void *, const Vector *, const QAngle *);
using UpdateLightFn = void(IVR_FASTCALL *)(void *, void *, int, const Vector *, const Vector *, const Vector *,
                                           const Vector *, float, float, float, bool, const char *);
using InfraFlashlightFn = void(IVR_FASTCALL *)(void *, void *, float, float, float, float, float, float, float);
using DrawPanelIn3DSpaceFn = void(IVR_FASTCALL *)(void *, void *, unsigned, const float *, int, int, float, float);
using BuildTransformationsFn = void(IVR_FASTCALL *)(void *, void *, void *, void *, void *, const void *, int, void *);
// ---- server ------------------------------------------------------------------------------
using UpdateObjectFn = bool(IVR_FASTCALL *)(void *, void *, void *, float, bool);
using UpdateObjectVMFn = bool(IVR_FASTCALL *)(void *, void *, void *, float);
using ComputeErrorFn = float(IVR_FASTCALL *)(void *, void *);
using EyeAnglesFn = const QAngle *(IVR_FASTCALL *)(void *, void *);
using ShootPositionFn = Vector *(IVR_FASTCALL *)(void *, void *, Vector *);
using FindUseEntityFn = void *(IVR_FASTCALL *)(void *, void *);
using GrabSetTargetFn = void(IVR_FASTCALL *)(void *, void *, const Vector *, const QAngle *, bool);

Hook<RenderViewFn> hkRenderView;
Hook<RenderCameraWeaponFn> hkRenderCameraWeapon;
Hook<CreateMoveFn> hkCreateMove;
Hook<CalcViewModelViewFn> hkCalcViewModelView;
Hook<UpdateLightFn> hkUpdateLight;
Hook<BuildTransformationsFn> hkBuildTransformations;
Hook<DrawPanelIn3DSpaceFn> hkDrawPanelIn3DSpace;
Hook<InfraFlashlightFn> hkInfraFlashlight;
Hook<UpdateObjectFn> hkUpdateObject;
Hook<UpdateObjectVMFn> hkUpdateObjectVM;
Hook<ComputeErrorFn> hkComputeError;
Hook<EyeAnglesFn> hkEyeAngles;
Hook<ShootPositionFn> hkShootPosition;
Hook<FindUseEntityFn> hkFindUseEntity;        // server
Hook<ShootPositionFn> hkEyePosition;          // server
Hook<GrabSetTargetFn> hkGrabSetTarget;        // server
using GrabDetachFn = void(IVR_FASTCALL *)(void *, void *, bool);
Hook<GrabDetachFn> hkGrabDetach;              // server
Hook<FindUseEntityFn> hkClientFindUseEntity;
Hook<ShootPositionFn> hkClientEyePosition;
Hook<EyeAnglesFn> hkClientEyeAngles;

void *g_eyeRT[2] = {nullptr, nullptr};
void *g_hudRT = nullptr;
int g_hudW = 0, g_hudH = 0;
bool g_texturesFailed = false;
int g_renderDepth = 0;
int g_currentEye = -1;
// The eye being drawn, for the hands drawn with the viewmodels (before post-processing).
CViewSetup g_eyeView{};
bool g_handsDrawn = false;
CViewSetup g_engineView; // the engine's own (flat) view of this frame
int g_grabOverride = 0;
double g_lastLog = 0;
double g_traceAgainAt = 0;
bool g_traceRequest = false;
// Traced on the main thread too: the screen-space rectangles INFRA's post-processing and selection
// glow draw (IMatRenderContext::DrawScreenSpaceRectangle, vtable 105), with their material.
bool g_traceRects = false;
bool g_dumpOutline = false; // this trace also saves pictures of the outline passes
using DrawSSRFn = void(IVR_FASTCALL *)(void *, void *, void *, int, int, int, int, float, float, float, float, int,
                                       int, void *, int, int);
Hook<DrawSSRFn> hkDrawSSR;
void IVR_FASTCALL DrawSSRDetour(void *self, void *edx, void *mat, int dx, int dy, int w, int h, float x0, float y0,
                                float x1, float y1, int tw, int th, void *rend, int xd, int yd)
{
    if (g_traceRects)
    {
        const char *n = mat ? VCall<const char *>(mat, vidx::MAT_GetName) : nullptr;
        IVR_LOG("  rect: %s dest %d,%d %dx%d from %.1f,%.1f-%.1f,%.1f of %dx%d", n ? n : "?", dx, dy, w, h, x0, y0,
                x1, y1, tw, th);
    }
    hkDrawSSR.original(self, edx, mat, dx, dy, w, h, x0, y0, x1, y1, tw, th, rend, xd, yd);
    if (g_traceRects && g_dumpOutline)
    {
        // Pictures of the selection glow's stages, for working out where it goes wrong.
        const char *n = mat ? VCall<const char *>(mat, vidx::MAT_GetName) : nullptr;
        if (n && std::strstr(n, "glow_downsample"))
            Defer([] { g_D3D.DumpBound(false, "outline_1_silhouette"); });
        else if (n && std::strstr(n, "glow_blur_y"))
            Defer([] { g_D3D.DumpBound(true, "outline_2_glow"); });
        else if (n && std::strstr(n, "halo_add_to_screen"))
            Defer([] {
                g_D3D.DumpBound(true, "outline_3_eye");
                g_D3D.DumpStencil("outline_4_stencil");
            });
    }
} // trace the next left eye (set when INFRA's selection outline first shows)

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

// --- subtitle panels -------------------------------------------------------------------------
// INFRA (Portal 2 branch) parents its subtitle panels, "HudCloseCaption" and "HudSubtitles", to
// the fullscreen viewport, which is painted with the UI panels (PAINT_UIPANELS) so subtitles show
// over the pause menu - not with the in-game HUD. They are found by name in the vgui tree and
// painted on their own (ISurface::PaintTraverseEx) for the subtitle panel, and hidden while the
// pause menu is painted for the phone screen.
struct CaptionPanels
{
    unsigned int panel[4] = {};
    int count = 0;
    int levelLoads = -1;
    double nextSearch = 0;
    bool logged = false;
} g_captions;

std::string PanelName(unsigned int p)
{
    const char *n = VCall<const char *, unsigned int>(g_Game.vguiPanel, vidx::Panel_GetName, p);
    return n ? n : "";
}

void FindCaptionPanels(unsigned int p, int depth, std::string path)
{
    if (!p || depth > 12 || g_captions.count >= 4)
        return;
    std::string name = PanelName(p);
    path += "/" + name;
    if (name == "HudCloseCaption" || name == "HudSubtitles")
    {
        g_captions.panel[g_captions.count++] = p;
        if (!g_captions.logged)
        {
            int x = 0, y = 0, w = 0, h = 0;
            VCall<void, unsigned int, int &, int &>(g_Game.vguiPanel, vidx::Panel_GetAbsPos, p, x, y);
            VCall<void, unsigned int, int &, int &>(g_Game.vguiPanel, vidx::Panel_GetSize, p, w, h);
            bool vis = VCall<bool, unsigned int>(g_Game.vguiPanel, vidx::Panel_IsVisible, p);
            IVR_LOG("Subtitles: vgui panel %s at %d,%d size %dx%d%s", path.c_str(), x, y, w, h, vis ? "" : " (hidden)");
        }
        return;
    }
    int n = VCall<int, unsigned int>(g_Game.vguiPanel, vidx::Panel_GetChildCount, p);
    for (int i = 0; i < n && i < 512; ++i)
        FindCaptionPanels(VCall<unsigned int, unsigned int, int>(g_Game.vguiPanel, vidx::Panel_GetChild, p, i),
                          depth + 1, path);
}

// The subtitle panels (searched again after each level load, and every few seconds until found).
int CaptionPanelCount()
{
    if (!g_Game.vguiPanel || !g_Game.surface)
        return 0;
    double now = NowSeconds();
    if (g_captions.levelLoads != g_Mod.m_levelLoads || (g_captions.count == 0 && now > g_captions.nextSearch))
    {
        g_captions.levelLoads = g_Mod.m_levelLoads;
        g_captions.nextSearch = now + 5.0;
        g_captions.count = 0;
        FindCaptionPanels(VCall<unsigned int>(g_Game.surface, vidx::GetEmbeddedPanel), 0, "");
        if (!g_captions.logged)
        {
            if (!g_captions.count)
                IVR_LOG("Subtitles: no HudCloseCaption/HudSubtitles panel found yet - the subtitle panel shows "
                        "the UI layer's subtitle region instead");
            g_captions.logged = g_captions.count > 0;
        }
    }
    return g_captions.count;
}

void SetCaptionsVisible(bool on, bool saved[4])
{
    for (int i = 0; i < g_captions.count; ++i)
    {
        unsigned int p = g_captions.panel[i];
        if (on)
        {
            if (saved[i])
                VCall<void, unsigned int, bool>(g_Game.vguiPanel, vidx::Panel_SetVisible, p, true);
        }
        else
        {
            saved[i] = VCall<bool, unsigned int>(g_Game.vguiPanel, vidx::Panel_IsVisible, p);
            if (saved[i])
                VCall<void, unsigned int, bool>(g_Game.vguiPanel, vidx::Panel_SetVisible, p, false);
        }
    }
}

// Paints only the subtitles into the HUD render target (falls back to the whole UI layer while
// the panels are not found; the subtitle region is cut out of it either way).
void PaintSubtitles(const CViewSetup *hudView)
{
    if (CaptionPanelCount() == 0)
    {
        PaintHud(hudView, PAINT_UIPANELS | PAINT_INGAMEPANELS);
        return;
    }
    void *ctx = g_Game.RenderContext();
    CViewSetup hv = *hudView;
    hv.x = hv.y = 0;
    hv.width = g_hudW;
    hv.height = g_hudH;
    alignas(16) uint8_t frustum[512] = {};
    VCall<void, void *, CViewSetup *, int, void *, void *>(g_Game.renderView, 41, ctx, &hv, 0, g_hudRT, frustum);
    VCall<void, bool, bool>(ctx, vidx::RC_OverrideAlphaWriteEnable, true, true);
    VCall<void, unsigned char, unsigned char, unsigned char, unsigned char>(ctx, vidx::RC_ClearColor4ub, 0, 0, 0, 0);
    VCall<void, bool, bool, bool>(ctx, vidx::RC_ClearBuffers, true, false, false);
    for (int i = 0; i < g_captions.count; ++i)
        VCall<void, unsigned int, bool>(g_Game.surface, vidx::PaintTraverseEx, g_captions.panel[i], false);
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
    // The shape of the camera's screen: the view is stretched over the whole screen texture.
    float aspect = hands::Enabled() ? hands::CameraScreenAspect() : 0.0f;
    if (aspect > 0.3f && aspect < 4.0f)
    {
        if ((float)w / (float)h > aspect)
            w = (int)(h * aspect);
        else
            h = (int)(w / aspect);
    }
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

    int w = (int)g_VR.ViewW(), h = (int)g_VR.ViewH();
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
        bool trace = eye == 0 && g_D3D.eye[0].Valid() &&
                     ((s_traced < 2 && (s_traced == 0 || NowSeconds() > g_traceAgainAt)) || g_traceRequest);
        g_dumpOutline = trace && g_traceRequest;
        if (trace && g_traceRequest)
        {
            g_traceRequest = false;
            Defer([] { IVR_LOG("trace: the selection outline is showing - tracing its passes"); });
        }
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
        if (trace && !hkDrawSSR.Installed())
        {
            if (void *ctx = g_Game.RenderContext())
            {
                void **vt = *reinterpret_cast<void ***>(ctx);
                hkDrawSSR.Install("IMatRenderContext::DrawScreenSpaceRectangle", vt[105], (void *)&DrawSSRDetour);
                Game::ReleaseContext(ctx);
            }
        }
        g_traceRects = trace;
        world::BeginEye(); // post-processing works out its rectangles for the eye, not the window
        g_eyeView = v;
        g_handsDrawn = false;
        hkRenderView.original(self, edx, &v, &hudEye, clearFlags,
                              flags | (eye == 1 ? RENDERVIEW_SUPPRESSMONITORRENDERING : 0));
        world::EndEye();
        g_traceRects = false;
        g_dumpOutline = false;
        if (!g_handsDrawn) // no viewmodel pass this eye (or hands.post_processing = false): on top
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

    bool hud = false, subs = false;
    // Subtitles get a panel of their own: the in-game HUD is painted on its own first and the
    // subtitle region copied out of it, then (phone menu) the pause menu is painted for the phone.
    bool subsLayer = g_hudRT && g_Mod.SubtitleLayer();
    if (g_hudRT && g_Mod.phoneMenu)
    {
        if (subsLayer)
        {
            PaintSubtitles(hudView);
            Defer([] { g_D3D.CopySubtitles(); });
            subs = true;
            // The pause menu for the phone screen, without the subtitles (they are UI panels too).
            bool saved[4] = {};
            bool hide = !g_Config.subtitlesOnPhone && CaptionPanelCount() > 0;
            if (hide)
                SetCaptionsVisible(false, saved);
            PaintHud(hudView, PAINT_UIPANELS | PAINT_INGAMEPANELS);
            if (hide)
                SetCaptionsVisible(true, saved);
        }
        else
            PaintHud(hudView, PAINT_UIPANELS | PAINT_INGAMEPANELS); // the pause menu, for the phone panel
        hud = true;
    }
    else if (g_Config.hudMode == HudMode::Overlay && g_hudRT)
    {
        if (subsLayer)
        {
            PaintSubtitles(hudView);
            Defer([] { g_D3D.CopySubtitles(); });
            subs = true;
        }
        hud = !subsLayer || g_Config.subtitlesHudPanel;
        if (hud)
            PaintHud(hudView, PAINT_INGAMEPANELS);
    }
    vr::HmdMatrix34_t pose = g_VR.Head().raw; // what this frame was drawn for
    Defer([hud, subs, pose] {
        g_Mod.subsPainted = subs;
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
    // The mod's hands go in with the viewmodels, so the engine's post-processing (bloom, tone
    // mapping, colour correction, screen effects) that follows applies to them too. They go first:
    // INFRA's screen panels (phone menu, camera viewfinder) draw additively onto the dark glass of
    // the mod's phone / camera, as they do on INFRA's own models.
    if (g_currentEye >= 0 && !g_handsDrawn && g_Config.handsPostProcessing && hands::Enabled())
    {
        hands::DrawEye(g_currentEye, g_eyeView);
        g_handsDrawn = true;
        static bool s_said = false;
        if (!s_said)
        {
            s_said = true;
            IVR_LOG("GLB hands: drawn just before the viewmodels, before post-processing");
        }
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

// [camera] / [flashlight] / [phone] model: while INFRA draws v_camera / v_light / v_phone, its animated bones drive the
// mod's item model (same bone names). Returns true if the bones were handed over.
// Returns the item whose model takes over this viewmodel, or -1.
int CaptureItemBones(const uint8_t *studio, const matrix3x4_t *bones, int count)
{
    const char *name = reinterpret_cast<const char *>(studio + 12);
    int item = std::strstr(name, "v_camera")  ? hands::ITEM_CAMERA
               : std::strstr(name, "v_light") ? hands::ITEM_FLASHLIGHT
               : std::strstr(name, "v_phone") ? hands::ITEM_PHONE
                                              : -1;
    if (item < 0 || !hands::ItemModelEnabled(item))
        return -1;
    const std::vector<std::string> &joints = hands::ItemJointNames(item);
    static const void *s_studio[hands::ITEM_COUNT] = {};
    static std::vector<int> s_map[hands::ITEM_COUNT];
    std::vector<int> &map = s_map[item];
    if (s_studio[item] != studio || map.size() != joints.size())
    {
        s_studio[item] = studio;
        map.assign(joints.size(), -1);
        int missing = 0;
        for (size_t j = 0; j < joints.size(); ++j)
            missing += (map[j] = FindBone(studio, joints[j].c_str())) < 0;
        static const char *kWhat[hands::ITEM_COUNT] = {"Camera", "Flashlight", "Phone"};
        IVR_LOG("%s model: %s has %d of the model's %u bones%s", kWhat[item],
                name, (int)joints.size() - missing, (unsigned)joints.size(),
                missing ? " - the others follow its hand" : "");
    }
    int hand = FindBone(studio, joints.empty() ? "" : joints[0].c_str()); // the hand joint comes first
    std::vector<GlbMat34> world(joints.size());
    for (size_t j = 0; j < joints.size(); ++j)
    {
        int b = map[j] >= 0 ? map[j] : hand;
        if (b < 0 || b >= count)
            return -1;
        std::memcpy(world[j].m, bones[b].m, sizeof(world[j].m));
    }
    hands::SetItemBones(item, world);
    return item;
}

// The last screen panel put on an item model (main thread): DrawPanelIn3DSpaceDetour scales it.
struct PanelTarget
{
    bool valid = false;
    Vector upperLeft, normal;
    float width = 0, height = 0, scale = 1;
    double at = 0;
};
PanelTarget g_panelTarget;
constexpr const char *kPanelScaleFlag = "panel_scale";

// IMatSystemSurface::DrawPanelIn3DSpace (C_VGuiScreen::DrawModel): the matrix's origin is the
// panel's upper left corner, its columns the panel's axes (x along its width, z out of it).
// INFRA's panel on the mod's phone / camera model is drawn `scale` times bigger, matching the
// model the way it matches INFRA's own viewmodel. Recognised by its corner, facing and size.
void IVR_FASTCALL DrawPanelIn3DSpaceDetour(void *self, void *edx, unsigned panel, const float *m, int pw, int ph,
                                           float w, float h)
{
    const PanelTarget &t = g_panelTarget;
    if (t.valid && m && NowSeconds() - t.at < 0.5 && std::fabs(w - t.width) < 0.01f && std::fabs(h - t.height) < 0.01f)
    {
        Vector origin(m[3], m[7], m[11]), z(m[2], m[6], m[10]);
        if ((origin - t.upperLeft).Length() < 0.5f && Dot(z.Normalized(), t.normal) > 0.98f)
        {
            static int s_draws = 0;
            if (s_draws == 0)
                IVR_LOG("Screen panel: drawn %.2f times INFRA's size on the mod's model", t.scale);
            if (++s_draws == 60) // a bad panel size would crash at once; INFRA closing later is fine
                flags::End(kPanelScaleFlag);
            w *= t.scale;
            h *= t.scale;
        }
    }
    hkDrawPanelIn3DSpace.original(self, edx, panel, m, pw, ph, w, h);
}

// INFRA's screen panels: C_VGuiScreen entities parented to the viewmodel's controlpanel%d_ll
// attachment, sized from the _ll -> _ur attachments (its menu on v_phone, the viewfinder on
// v_camera). Attachments are worked out from the bones after BuildTransformations, so the bones
// carrying them decide where INFRA draws the panel.
struct ScreenPanel
{
    int bone = -1;
    Vector ll;     // lower left corner, in the bone's space
    Mat3 frame;    // the attachment's axes in the bone's space (x width, y up, z out)
    Vector centre; // the panel's centre, in the bone's space
    float width = 0, height = 0;
};

constexpr int kStudioAttachmentSize = 92; // mstudioattachment_t: name, flags, localbone, matrix3x4_t local, unused[8]

bool FindScreenPanel(const uint8_t *studio, ScreenPanel &out)
{
    int count = *reinterpret_cast<const int *>(studio + 240);  // numlocalattachments
    int index = *reinterpret_cast<const int *>(studio + 244);  // localattachmentindex
    int bones = *reinterpret_cast<const int *>(studio + 156);
    if (count <= 0 || count > 64 || index <= 0 || !IsReadable(studio + index, count * kStudioAttachmentSize))
        return false;
    const uint8_t *ll = nullptr, *ur = nullptr;
    for (int i = 0; i < count; ++i)
    {
        const uint8_t *a = studio + index + i * kStudioAttachmentSize;
        const char *name = reinterpret_cast<const char *>(a + *reinterpret_cast<const int *>(a));
        if (std::strcmp(name, "controlpanel0_ll") == 0)
            ll = a;
        else if (std::strcmp(name, "controlpanel0_ur") == 0)
            ur = a;
    }
    if (!ll || !ur)
        return false;
    int bone = *reinterpret_cast<const int *>(ll + 8);
    if (bone < 0 || bone >= bones || bone != *reinterpret_cast<const int *>(ur + 8))
        return false;
    const float *ml = reinterpret_cast<const float *>(ll + 12), *mu = reinterpret_cast<const float *>(ur + 12);
    out.bone = bone;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.frame.m[r][c] = ml[r * 4 + c];
    out.ll = Vector(ml[3], ml[7], ml[11]);
    Vector u(mu[3], mu[7], mu[11]);
    Vector d = out.frame.Transposed() * (u - out.ll); // the upper right corner in the panel's frame
    out.width = d.x;
    out.height = d.y;
    out.centre = (out.ll + u) * 0.5f;
    return out.width > 0.01f && out.height > 0.01f;
}

// With the mod's item model drawn: the bone carrying INFRA's panel (no vertices are skinned to it in
// v_phone or v_camera) is turned and placed so that the panel's attachment lands on the model's
// screen, centred on it. Returns that bone (or -1); `point` becomes the panel's corner,
// where the rest of the hidden viewmodel collapses to.
int PlaceScreenPanel(const uint8_t *studio, int item, matrix3x4_t *bones, int count, float point[3])
{
    static const void *s_studio[hands::ITEM_COUNT] = {};
    static ScreenPanel s_panel[hands::ITEM_COUNT];
    static bool s_have[hands::ITEM_COUNT] = {};
    if (s_studio[item] != studio)
    {
        s_studio[item] = studio;
        s_have[item] = FindScreenPanel(studio, s_panel[item]);
        const char *name = reinterpret_cast<const char *>(studio + 12);
        if (s_have[item])
            IVR_LOG("Screen panel: %s has INFRA's screen on bone %d (%.2f x %.2f units) - moved onto the mod's model",
                    name, s_panel[item].bone, s_panel[item].width, s_panel[item].height);
        else
            IVR_LOG("Screen panel: %s has no screen panel", name);
    }
    if (!s_have[item])
        return -1;
    const ScreenPanel &sp = s_panel[item];
    if (sp.bone >= count)
        return -1;
    int index = *reinterpret_cast<const int *>(studio + 160);
    const uint8_t *b = studio + index + sp.bone * kStudioBoneSize;
    const char *boneName = reinterpret_cast<const char *>(b + *reinterpret_cast<const int *>(b));
    Pose panel;
    float scale = 1.0f;
    if (!hands::ItemPanel(item, boneName, sp.centre, sp.frame, panel, scale))
        return -1;
    // INFRA sizes the panel for its own (smaller) viewmodel; DrawPanelIn3DSpaceDetour scales it up
    // by the model's scale from its upper left corner (the engine draws from there: the lower left
    // corner plus INFRA's height), so the corner is set for the scaled panel to sit centred.
    Vector x = panel.rot.Col(0), y = panel.rot.Col(1);
    Vector upperLeft = panel.pos - x * (sp.width * scale * 0.5f) + y * (sp.height * scale * 0.5f);
    Vector corner = upperLeft - y * sp.height;
    g_panelTarget = {true, upperLeft, panel.rot.Col(2), sp.width, sp.height, scale, NowSeconds()};
    // bone * attachment = (corner, panel axes): bone axes = panel axes * frame^T.
    Mat3 rot = panel.rot * sp.frame.Transposed();
    Vector t = corner - rot * sp.ll;
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
            bones[sp.bone].m[r][c] = rot.m[r][c];
        point[r] = (&corner.x)[r];
    }
    bones[sp.bone].m[0][3] = t.x;
    bones[sp.bone].m[1][3] = t.y;
    bones[sp.bone].m[2][3] = t.z;
    return sp.bone;
}

// [hands] model = glb: the mod draws its own hands, so INFRA's arms go - every bone outside the held
// item (camera, phone, flashlight) collapses to a point, which hides the vertices skinned to it.
// With the mod's camera or flashlight model, all of the viewmodel goes: the mod draws the item and the hand.
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
    int item = CaptureItemBones(studio, bones, count);
    bool itemModel = item >= 0;
    int items[3] = {FindBone(studio, "Camera"), FindBone(studio, "cellular_phone"), FindBone(studio, "flashlight")};
    // Hidden bones all collapse to one point. Each to its own point left thin triangles between
    // them (vertices weighted to several hidden bones), seen as dark slivers by the hand.
    float point[3] = {bones[0].m[0][3], bones[0].m[1][3], bones[0].m[2][3]};
    for (int it : items)
    {
        if (!itemModel && it >= 0 && it < count)
        {
            for (int r = 0; r < 3; ++r)
                point[r] = bones[it].m[r][3];
            break;
        }
    }
    // INFRA's screen panel (phone menu, camera viewfinder) goes onto the mod's model; the hidden
    // bones collapse at its corner so nothing of the viewmodel is left visible next to it.
    int panelBone = itemModel ? PlaceScreenPanel(studio, item, bones, count, point) : -1;
    int hidden = 0;
    for (int b = 0; b < count; ++b)
    {
        if (b == panelBone)
        {
            ++hidden;
            continue;
        }
        bool keep = false;
        // With the mod's item model nothing of INFRA's viewmodel is left to draw.
        for (int it : items)
            keep = keep || (!itemModel && it >= 0 && InSubtree(studio, b, it));
        if (keep)
            continue;
        for (int r = 0; r < 3; ++r)
        {
            bones[b].m[r][0] = bones[b].m[r][1] = bones[b].m[r][2] = 0.0f;
            bones[b].m[r][3] = point[r];
        }
        ++hidden;
    }
    static const void *s_logged = nullptr;
    if (s_logged != studio)
    {
        s_logged = studio;
        IVR_LOG("GLB hands: %s - %d of %d bones hidden (%s)", name, hidden, count,
                itemModel ? "all of it: the mod's item model is drawn" : "INFRA's arms; item bones kept");
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

// Where the flashlight beam starts and points: the lens of the mod's flashlight model while it is
// drawn, else the flashlight hand with [viewmodel] flashlight_offset.
Pose FlashlightPose()
{
    Pose p;
    if (hands::FlashlightBeam(p))
        return p;
    p = g_Mod.AimWorld(g_Mod.flashlightHand);
    p.pos = p.pos + p.rot * g_Config.flashlightOffset;
    return p;
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
        Pose p = FlashlightPose();
        Vector P = p.pos;
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
        Pose p = FlashlightPose();
        Vector P = p.pos;
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
// CBaseEntity::m_vecAbsOrigin (server): read by FindUseEntity / UpdateObject at +0x1cc after
// CalcAbsolutePosition when the dirty bit (0x800 in +0xc8) is set.
constexpr size_t kEntAbsOrigin = 0x1cc;
constexpr size_t kEntAbsAngles = 0x1d8; // CBaseEntity::m_angAbsRotation (datadesc)
void *g_grabPlayer = nullptr; // the player whose held object is being updated

// The server's entity list (CGlobalEntityList entries of 24 bytes: entity at +4, serial at +8),
// read from UpdateObject's own "mov edx, [g_pEntityList]" at +0x47 (8B 15 imm32).
const uint8_t *ServerEntityArray()
{
    static const uint8_t *s_list = nullptr;
    static bool s_tried = false;
    if (!s_tried)
    {
        s_tried = true;
        const uint8_t *p = static_cast<const uint8_t *>(g_Game.fnGrabUpdateObject);
        if (p && IsReadable(p + 0x47, 6) && p[0x47] == 0x8B && p[0x48] == 0x15)
            s_list = *reinterpret_cast<const uint8_t *const *>(p + 0x49);
        IVR_LOG("Server entity list %s", s_list ? "found" : "not found (no touch use, no object diagnostics)");
    }
    if (!s_list || !IsReadable(s_list, 4))
        return nullptr;
    return *reinterpret_cast<const uint8_t *const *>(s_list);
}

void *ServerEntity(uint32_t handle)
{
    const uint8_t *base = ServerEntityArray();
    if (!base || handle == 0xFFFFFFFFu)
        return nullptr;
    const uint8_t *e = base + (handle & 0xFFFF) * 24 + 4;
    if (!IsReadable(e, 8) || *reinterpret_cast<const uint32_t *>(e + 4) != (handle >> 16))
        return nullptr;
    void *ent = *reinterpret_cast<void *const *>(e);
    return IsReadable(static_cast<uint8_t *>(ent) + kEntAbsOrigin, 12) ? ent : nullptr;
}

bool IVR_FASTCALL UpdateObjectDetour(void *self, void *edx, void *player, float error, bool teleport)
{
    g_grabPlayer = player;
    g_Mod.lastGrabUpdate = NowSeconds();
    ++g_grabOverride;
    bool r = hkUpdateObject.original(self, edx, player, error, teleport);
    --g_grabOverride;
    return r;
}

bool IVR_FASTCALL UpdateObjectVMDetour(void *self, void *edx, void *player, float error)
{
    g_grabPlayer = player;
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

// ---- hand-based "use": the game's use search starts at the hand -----------------------------
// INFRA's FindUseEntity (server: what "use" acts on; client: what the selection outline marks)
// traces from EyePosition() along EyeVectors() (= EyeAngles()), then looks around that point.
// While it runs for the player, both answer with the hand instead.
void *g_usePlayer = nullptr; // server player whose FindUseEntity is running with the hand
Vector g_useStart;
QAngle g_useAngles;
void *g_clUsePlayer = nullptr; // the same on the client
Vector g_clUseStart;
QAngle g_clUseAngles;
constexpr const char *kHandUseFlag = "hand_use";
constexpr const char *kHoldInHandFlag = "hold_in_hand";

// CBaseEntity::m_Collision (CCollisionProperty, datadesc +0xe8): m_vecMins +0x08, m_vecMaxs +0x14 (in
// the entity's space, or world-aligned for SOLID_BBOX), m_nSolidType byte +0x22.
constexpr size_t kEntCollision = 0xe8;
constexpr size_t kEntCollisionGroup = 0x148; // CBaseEntity::m_CollisionGroup (datadesc)
constexpr int kSolidBBox = 2;
using CanUseFn = bool(IVR_FASTCALL *)(void *, void *, void *);

// Distance (game units) from P to a collision box (mins/maxs in the entity's space, or
// world-aligned for SOLID_BBOX); < 0 if the numbers make no sense.
float BoxDistance(const Vector &o, const QAngle &a, const Vector &mn, const Vector &mx, int solid, const Vector &P,
                  float *volume)
{
    const float v[] = {o.x, o.y, o.z, a.x, a.y, a.z, mn.x, mn.y, mn.z, mx.x, mx.y, mx.z};
    for (float f : v)
        if (!std::isfinite(f))
            return -1.0f;
    if (mx.x < mn.x || mx.y < mn.y || mx.z < mn.z || (mx - mn).Length() > 1024.0f)
        return -1.0f;
    Vector d = P - o;
    if (solid != kSolidBBox)
        d = AnglesToMatrix(a).Transposed() * d;
    Vector c(std::clamp(d.x, mn.x, mx.x), std::clamp(d.y, mn.y, mx.y), std::clamp(d.z, mn.z, mx.z));
    if (volume)
        *volume = (mx.x - mn.x) * (mx.y - mn.y) * (mx.z - mn.z);
    return (d - c).Length();
}

// Server entity (from the game's own list, so live): its collision box.
float DistanceToBox(const uint8_t *ent, const Vector &P, float *volume)
{
    return BoxDistance(*reinterpret_cast<const Vector *>(ent + kEntAbsOrigin),
                       *reinterpret_cast<const QAngle *>(ent + kEntAbsAngles),
                       *reinterpret_cast<const Vector *>(ent + kEntCollision + 0x08),
                       *reinterpret_cast<const Vector *>(ent + kEntCollision + 0x14), ent[kEntCollision + 0x22], P,
                       volume);
}

// Doors and valves (momentary_rot_button / func_rot_button: CMomentaryRotButton, CRotButton and
// their client classes) are left to the grip and the trigger: the fingertip skips them.
bool IsDoor(const void *ent)
{
    const char *n = ent ? RttiName(ent) : "";
    return n && (strstr(n, "Door") || strstr(n, "RotButton"));
}

// What a hand (or fingertip) is touching: the nearest entity the game would let you use whose
// collision box is within `reach` of one of the points. Small things are hard to hit with the
// game's traces and cone; this goes by the box instead. Smaller boxes win ties, so a note on a
// usable desk is picked over the desk.
// Runs every tick while "use" is held, so it stays cheap: the list is checked readable once (the
// entities in it are live) and the answer is kept for 0.1 s while the hand stays put.
void *TouchUseEntity(void *player, const Vector *points, int count, float reach, bool noDoors)
{
    static const uint8_t *s_checked = nullptr;
    const uint8_t *base = ServerEntityArray();
    if (!base || !g_Game.fnPlayerCanUse || count <= 0 || reach <= 0.0f)
        return nullptr;
    if (base != s_checked)
    {
        if (!IsReadable(base, 2048 * 24))
            return nullptr;
        s_checked = base;
    }
    static double s_at = -1.0;
    static Vector s_point;
    static void *s_result = nullptr;
    static uint32_t s_serial = 0;
    static int s_index = -1;
    static bool s_noDoors = false;
    const double now = NowSeconds();
    if (now - s_at < 0.1 && (points[0] - s_point).Length() < 1.0f && noDoors == s_noDoors)
    {
        if (!s_result)
            return nullptr;
        const uint8_t *e = base + s_index * 24 + 4;
        if (*reinterpret_cast<void *const *>(e) == s_result && *reinterpret_cast<const uint32_t *>(e + 4) == s_serial)
            return s_result;
    }
    void *best = nullptr;
    int bestIndex = -1;
    float bestD = 1e9f, bestV = 1e9f;
    for (int i = 1; i < 2048; ++i)
    {
        const uint8_t *ent = *reinterpret_cast<const uint8_t *const *>(base + i * 24 + 4);
        if (!ent || ent == player)
            continue;
        const Vector o = *reinterpret_cast<const Vector *>(ent + kEntAbsOrigin);
        if (!std::isfinite(o.x) || (o - points[0]).Length() > 256.0f)
            continue;
        float d = 1e9f, v = 0;
        for (int k = 0; k < count; ++k)
        {
            float vk = 0, dk = DistanceToBox(ent, points[k], &vk);
            if (dk >= 0.0f && dk < d)
            {
                d = dk;
                v = vk;
            }
        }
        if (d > reach || d > bestD + 0.5f || (d >= bestD - 0.5f && v >= bestV))
            continue;
        if (!reinterpret_cast<CanUseFn>(g_Game.fnPlayerCanUse)(player, nullptr, const_cast<uint8_t *>(ent)))
            continue;
        if (noDoors && IsDoor(ent))
            continue;
        best = const_cast<uint8_t *>(ent);
        bestIndex = i;
        bestD = d;
        bestV = v;
    }
    s_at = now;
    s_point = points[0];
    s_noDoors = noDoors;
    s_result = best;
    s_index = bestIndex;
    s_serial = best ? *reinterpret_cast<const uint32_t *>(base + bestIndex * 24 + 8) : 0;
    return best;
}

void *IVR_FASTCALL FindUseEntityDetour(void *self, void *edx)
{
    if (g_usePlayer || !g_Mod.Active() || g_Mod.menuMode || !g_Mod.HandUseActive())
        return hkFindUseEntity.original(self, edx);
    // First what the hand is right at: short searches through the hand in several directions
    // (along it, down, and across the palm both ways), kept only if the entity's origin is within
    // grab_reach of the hand. Then where the hand points.
    const int hand = g_Mod.interactHand;
    const float upm = g_Mod.UnitsPerMetre();
    if (g_Mod.PointerUseActive())
    {
        // The fingertip touched something: use only what it touches, never a door or valve.
        Vector tip = g_Mod.PointerTip(g_Mod.pointerHand);
        void *t = TouchUseEntity(self, &tip, 1, (g_Config.pointerReach + 0.03f) * upm, true);
        static int s_tipLogs = 0;
        if (s_tipLogs < 16)
        {
            ++s_tipLogs;
            IVR_LOG("Pointer touch (%s hand) at (%.0f %.0f %.0f): %s", g_Mod.pointerHand ? "right" : "left", tip.x,
                    tip.y, tip.z, t ? (RttiName(t)[0] ? RttiName(t) : "an entity") : "nothing usable");
        }
        return t;
    }
    void *r = nullptr;
    const bool touched = false;
    if (!r && g_Mod.HandValid(hand) && g_Config.grabReach > 0.0f)
    {
        const Vector P = g_Mod.handPos[hand];
        const Mat3 &R = g_Mod.handRot[hand];
        const Vector dirs[6] = {R.Forward(), Vector(0, 0, -1), R.Up() * -1.0f, R.Left(), R.Left() * -1.0f, R.Up()};
        const float reach = g_Config.grabReach * g_Mod.UnitsPerMetre();
        for (const Vector &d : dirs)
        {
            g_usePlayer = self;
            g_useStart = P - d * 12.0f;
            g_useAngles = VectorAngles(d);
            void *e = hkFindUseEntity.original(self, edx);
            g_usePlayer = nullptr;
            if (!e || !IsReadable(static_cast<uint8_t *>(e) + kEntAbsOrigin, 12))
                continue;
            Vector o = *reinterpret_cast<const Vector *>(static_cast<uint8_t *>(e) + kEntAbsOrigin);
            if ((o - P).Length() <= reach)
            {
                r = e;
                break;
            }
        }
    }
    if (!r)
    {
        g_usePlayer = self;
        g_useStart = g_Mod.useStart;
        g_useAngles = g_Mod.useAngles;
        r = hkFindUseEntity.original(self, edx);
        g_usePlayer = nullptr;
    }
    static int s_logs = 0;
    static void *s_last = nullptr;
    if (s_logs < 16 && (r != s_last || !r))
    {
        ++s_logs;
        s_last = r;
        IVR_LOG("Hand use (%s hand) %s (%.0f %.0f %.0f) aim %.0f %.0f: %s", g_Mod.interactHand ? "right" : "left",
                touched ? "touching, at" : "from", touched ? g_Mod.handPos[hand].x : g_useStart.x,
                touched ? g_Mod.handPos[hand].y : g_useStart.y, touched ? g_Mod.handPos[hand].z : g_useStart.z,
                g_useAngles.x, g_useAngles.y, r ? (RttiName(r)[0] ? RttiName(r) : "an entity") : "nothing usable");
    }
    return r;
}

Vector *IVR_FASTCALL EyePositionDetour(void *self, void *edx, Vector *out)
{
    Vector *r = hkEyePosition.original(self, edx, out);
    if (g_usePlayer && self == g_usePlayer && r)
        *r = g_useStart;
    return r;
}

// Client entity: IClientUnknown::GetCollideable (vtable 3) -> ICollideable OBBMins (1), OBBMaxs (2),
// GetCollisionOrigin (8), GetCollisionAngles (9), GetSolid (11); read from INFRA's client.dll
// (C_BaseEntity vt[3] = this + 0x410, CCollisionProperty's vtable).
float ClientDistanceToBox(void *ent, const Vector &P)
{
    if (!ent)
        return -1.0f;
    void *coll = VCall<void *>(ent, 3);
    if (!coll)
        return -1.0f;
    const Vector *mn = VCall<const Vector *>(coll, 1);
    const Vector *mx = VCall<const Vector *>(coll, 2);
    const Vector *o = VCall<const Vector *>(coll, 8);
    const QAngle *a = VCall<const QAngle *>(coll, 9);
    int solid = VCall<int>(coll, 11);
    if (!mn || !mx || !o || !a)
        return -1.0f;
    return BoxDistance(*o, *a, *mn, *mx, solid, P, nullptr);
}

// A small item (switch plate, valve, cassette player): the outline problem shows on these, not
// on doors, so the outline trace and image dump wait for one.
bool IsSmallClientEntity(void *ent)
{
    void *coll = ent ? VCall<void *>(ent, 3) : nullptr;
    const Vector *mn = coll ? VCall<const Vector *>(coll, 1) : nullptr;
    const Vector *mx = coll ? VCall<const Vector *>(coll, 2) : nullptr;
    if (!mn || !mx)
        return false;
    Vector size = *mx - *mn;
    return std::max(size.x, std::max(size.y, size.z)) < 40.0f;
}

// The pointer finger: with only the index finger out, what the outline search finds from the
// fingertip is touched when the tip reaches its box (never a door or valve); the first touch presses
// "use" once (OnCreateMove), and the tip has to come off it before it can press again.
void *PointerSearch(void *self, void *edx)
{
    void *outline = nullptr;
    const float upm = g_Mod.UnitsPerMetre();
    for (int h = 0; h < 2; ++h)
    {
        if (!g_Mod.IsPointing(h) || h == g_Mod.HeldObjectHand())
        {
            g_Mod.pointerTouching[h] = false;
            continue;
        }
        const Vector tip = g_Mod.PointerTip(h);
        const Vector fwd = g_Mod.handRot[h].Forward();
        g_clUseStart = tip - fwd * 4.0f;
        g_clUseAngles = VectorAngles(fwd);
        g_clUsePlayer = self;
        void *e = hkClientFindUseEntity.original(self, edx);
        g_clUsePlayer = nullptr;
        float d = e && !IsDoor(e) ? ClientDistanceToBox(e, tip) : -1.0f;
        static double s_logAt[2] = {0, 0};
        if (NowSeconds() - s_logAt[h] > 2.0)
        {
            static int s_logs = 0;
            s_logAt[h] = NowSeconds();
            if (s_logs++ < 30)
                IVR_LOG("Pointer (%s hand): fingertip (%.0f %.0f %.0f), %s%s %.1f cm away",
                        h ? "right" : "left", tip.x, tip.y, tip.z, e ? (RttiName(e)[0] ? RttiName(e) : "an entity") : "nothing",
                        e && IsDoor(e) ? " (door/valve, skipped)" : "", d >= 0.0f ? d / upm * 100.0f : -1.0f);
        }
        if (d >= 0.0f && d <= g_Config.pointerReach * upm)
        {
            if (!g_Mod.pointerTouching[h])
            {
                g_Mod.pointerPress[h] = true;
                g_VR.Haptic(h, 0.04f, 180.0f, 0.9f); // a click under the fingertip
                IVR_LOG("Pointer (%s hand): touched %s", h ? "right" : "left", RttiName(e)[0] ? RttiName(e) : "an entity");
            }
            g_Mod.pointerTouching[h] = true;
        }
        else if (d < 0.0f || d > (g_Config.pointerReach + 0.02f) * upm)
            g_Mod.pointerTouching[h] = false;
        if (!outline && d >= 0.0f && d <= 0.15f * upm)
            outline = e;
    }
    return outline;
}

void *IVR_FASTCALL ClientFindUseEntityDetour(void *self, void *edx)
{
    if (g_clUsePlayer || !g_Mod.Active() || g_Mod.menuMode || !g_Config.handUse || !g_Mod.handUseClient)
        return hkClientFindUseEntity.original(self, edx);
    if (void *p = g_Config.pointerTouch ? PointerSearch(self, edx) : nullptr)
        return p;
    // The outline marks what either hand points at: the hand that used something last first.
    int order[2] = {g_Mod.interactHand, 1 - g_Mod.interactHand};
    void *r = nullptr;
    bool any = false;
    for (int h : order)
    {
        if (!g_Mod.HandValid(h) || h == g_Mod.HeldObjectHand())
            continue;
        any = true;
        g_Mod.UseRay(h, g_clUseStart, g_clUseAngles);
        g_clUsePlayer = self;
        r = hkClientFindUseEntity.original(self, edx);
        g_clUsePlayer = nullptr;
        // No outline on the object you are holding.
        if (r && g_Mod.HeldObjectHand() >= 0 && g_Mod.heldEntIndex > 0 && g_Game.entityList &&
            VCall<void *, int>(g_Game.entityList, vidx::GetClientEntity, g_Mod.heldEntIndex) == r)
            r = nullptr;
        if (r)
            break;
    }
    if (!any)
        return g_Mod.HeldObjectHand() >= 0 ? nullptr : hkClientFindUseEntity.original(self, edx);
    static int s_outlineTraces = 0;
    static double s_outlineTraceAt = 0;
    if (r && s_outlineTraces < 2 && NowSeconds() > s_outlineTraceAt && IsSmallClientEntity(r))
    {
        ++s_outlineTraces;
        s_outlineTraceAt = NowSeconds() + 30.0;
        g_traceRequest = true;
    }
    static int s_calls = 0;
    if (++s_calls == 600)
    {
        flags::End(kHandUseFlag);
        IVR_LOG("Hand use: the selection outline has followed the hands for 600 frames");
    }
    return r;
}

Vector *IVR_FASTCALL ClientEyePositionDetour(void *self, void *edx, Vector *out)
{
    Vector *r = hkClientEyePosition.original(self, edx, out);
    if (g_clUsePlayer && self == g_clUsePlayer && r)
        *r = g_clUseStart;
    return r;
}

const QAngle *IVR_FASTCALL ClientEyeAnglesDetour(void *self, void *edx)
{
    if (g_clUsePlayer && self == g_clUsePlayer)
        return &g_clUseAngles;
    return hkClientEyeAngles.original(self, edx);
}

// ---- held objects go to the hand ----------------------------------------------------------------
// CGrabController (Portal 2 branch, read from UpdateObject / SetTargetPosition in INFRA's
// server.dll): m_attachedEntity handle at +0x2054, m_attachedPositionObjectSpace (the grabbed
// point in the object's space) at +0x2078. UpdateObject ends with
// SetTargetPosition(end - R * m_attachedPositionObjectSpace, R): the grabbed point at `end`.
constexpr size_t kGrabAttachedEntity = 0x2054;
constexpr size_t kGrabAttachedPosition = 0x2078;
struct HoldState
{
    uint32_t handle = 0xFFFFFFFFu;
    int hand = -1;
    Mat3 rel;   // object rotation in the hand's frame, captured when it was picked up
    Mat3 world; // object rotation in the world when it was picked up (hold_rotation = keep)
    float yaw = 0; // which way it faced around the hand's up axis when picked up (hold_rotation = upright)
    int frames = 0;
    int logs = 0;
    bool onTrial = false;
} g_hold;

// The object's heading (degrees) around the hand's up axis, given its rotation in the hand's frame:
// its forward axis flattened onto the hand's forward/left plane, or its left axis when it points
// along the hand's up.
float UprightYaw(const Mat3 &rel)
{
    Vector f = rel.Forward();
    if (f.x * f.x + f.y * f.y > 0.04f)
        return std::atan2(f.y, f.x) * 57.29578f;
    Vector l = rel.Left();
    return std::atan2(l.y, l.x) * 57.29578f - 90.0f;
}

// Held objects must not collide with the player. INFRA's AttachEntity already moves the object to
// COLLISION_GROUP_PLAYER_HELD (23, which the game rules keep apart from the player and the player's
// movement) unless player_held_object_collide_with_player is set; make sure it is, and put the old
// group back when the object is let go if it was this code that changed it.
constexpr int kGroupPlayerHeld = 23;
struct NoCollideState
{
    uint32_t handle = 0xFFFFFFFFu; // the object this code moved to the held group
    int saved = -1;                // its group before
} g_noCollide;
using SetCollisionGroupFn = void(IVR_FASTCALL *)(void *, void *, int);

void RestoreHeldCollision()
{
    if (g_noCollide.handle == 0xFFFFFFFFu)
        return;
    uint8_t *ent = static_cast<uint8_t *>(ServerEntity(g_noCollide.handle));
    if (ent && g_Game.fnSetCollisionGroup && IsReadable(ent + kEntCollisionGroup, 4) &&
        *reinterpret_cast<int *>(ent + kEntCollisionGroup) == kGroupPlayerHeld)
        reinterpret_cast<SetCollisionGroupFn>(g_Game.fnSetCollisionGroup)(ent, nullptr, g_noCollide.saved);
    g_noCollide = NoCollideState();
}

void HeldNoCollide(uint32_t handle)
{
    RestoreHeldCollision();
    uint8_t *ent = static_cast<uint8_t *>(ServerEntity(handle));
    if (!ent || !IsReadable(ent + kEntCollisionGroup, 4))
        return;
    int group = *reinterpret_cast<int *>(ent + kEntCollisionGroup);
    int cvar = -1;
    if (g_Game.heldCollideCvar && IsReadable(g_Game.heldCollideCvar, 4))
        cvar = VCall<int>(g_Game.heldCollideCvar, 13); // ConVar::GetInt
    int playerGroup = -1;
    if (g_grabPlayer && IsReadable(static_cast<uint8_t *>(g_grabPlayer) + kEntCollisionGroup, 4))
        playerGroup = *reinterpret_cast<int *>(static_cast<uint8_t *>(g_grabPlayer) + kEntCollisionGroup);
    bool forced = false;
    if (g_Config.heldNoCollide && group != kGroupPlayerHeld && g_Game.fnSetCollisionGroup)
    {
        reinterpret_cast<SetCollisionGroupFn>(g_Game.fnSetCollisionGroup)(ent, nullptr, kGroupPlayerHeld);
        g_noCollide.handle = handle;
        g_noCollide.saved = group;
        forced = true;
    }
    IVR_LOG("Hold in hand: object collision group %d%s (player %d, player_held_object_collide_with_player %d)",
            group, forced ? ", moved to 23 so it passes through you" : "", playerGroup, cvar);
}

void IVR_FASTCALL GrabDetachDetour(void *self, void *edx, bool clearVelocity)
{
    hkGrabDetach.original(self, edx, clearVelocity);
    RestoreHeldCollision();
    g_hold.handle = 0xFFFFFFFFu;
}

void IVR_FASTCALL GrabSetTargetDetour(void *self, void *edx, const Vector *target, const QAngle *angles, bool teleport)
{
    const uint8_t *c = static_cast<const uint8_t *>(self);
    if (g_grabOverride <= 0 || !g_Mod.Active() || g_Mod.menuMode || !g_Config.holdInHand || !target || !angles ||
        !g_Mod.HandValid(g_Mod.interactHand) || !IsReadable(c + kGrabAttachedEntity, 0x30))
        return hkGrabSetTarget.original(self, edx, target, angles, teleport);
    uint32_t handle = *reinterpret_cast<const uint32_t *>(c + kGrabAttachedEntity);
    g_Mod.heldEntIndex = handle == 0xFFFFFFFFu ? -1 : (int)(handle & 0xFFFF);
    Vector local = *reinterpret_cast<const Vector *>(c + kGrabAttachedPosition);
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || local.Length() > 512.0f)
        local = Vector(0, 0, 0);
    // On trial only while it actually moves objects, so a crash somewhere else in a session where
    // nothing was held does not switch it off.
    if (!g_hold.onTrial && g_hold.frames < 300)
    {
        if (!flags::Begin(kHoldInHandFlag))
            return hkGrabSetTarget.original(self, edx, target, angles, teleport);
        g_hold.onTrial = true;
    }
    Mat3 game = AnglesToMatrix(*angles);
    if (handle != g_hold.handle)
        HeldNoCollide(handle);
    if (handle != g_hold.handle || g_Mod.interactHand != g_hold.hand)
    {
        // Picked up (or passed to the other hand): keep the way it is turned now, relative to the hand.
        g_hold.handle = handle;
        g_hold.hand = g_Mod.interactHand;
        // Start from how the object is really turned, not the angles the game asks for: INFRA's
        // grab controller snaps props to its preferred carry angles, which turned them on pickup.
        Mat3 now = game;
        if (const uint8_t *ent = static_cast<const uint8_t *>(ServerEntity(handle)))
        {
            if (IsReadable(ent + kEntAbsAngles, sizeof(QAngle)))
            {
                QAngle real = *reinterpret_cast<const QAngle *>(ent + kEntAbsAngles);
                if (std::isfinite(real.x) && std::isfinite(real.y) && std::isfinite(real.z))
                    now = AnglesToMatrix(real);
            }
        }
        g_hold.world = now;
        g_hold.rel = g_Mod.holdRot.Transposed() * now;
        g_hold.yaw = UprightYaw(g_hold.rel);
        Vector d = *target + game * local - g_Mod.holdPos;
        IVR_LOG("Hold in hand: object %08x in the %s hand (grab point %.0f %.0f %.0f in the object, the game held "
                "it %.0f units from the hand)",
                handle, g_hold.hand ? "right" : "left", local.x, local.y, local.z, d.Length());
    }
    Mat3 R = g_Config.holdRotation == Config::HoldKeep   ? g_hold.world
             : g_Config.holdRotation == Config::HoldHand ? g_Mod.holdRot * g_hold.rel
                                                         : g_Mod.holdRot * AnglesToMatrix(QAngle(0, g_hold.yaw, 0));
    QAngle a = MatrixToAngles(R);
    // The hold point relative to the player's eye on the server this tick: the client's view origin
    // is interpolated and lags the server while moving, which made held objects hitch.
    Vector hold = g_Mod.holdPos;
    if (g_grabPlayer && hkEyePosition.Installed())
    {
        Vector eye;
        Vector *pe = hkEyePosition.original(g_grabPlayer, nullptr, &eye);
        if (pe && std::isfinite(pe->x) && (*pe + g_Mod.holdRel - g_Mod.holdPos).Length() < 64.0f)
            hold = *pe + g_Mod.holdRel;
    }
    Vector t = hold - R * local;
    // Diagnostics: how far the object really is from where it is being held.
    if (g_grabPlayer && (g_hold.frames % 45) == 0 && g_hold.logs < 40)
    {
        Vector eye;
        Vector *pe = hkEyePosition.original(g_grabPlayer, nullptr, &eye);
        Vector rel = pe ? hold - *pe : Vector(0, 0, 0);
        float off = -1.0f;
        if (const uint8_t *ent = static_cast<const uint8_t *>(ServerEntity(handle)))
            off = (*reinterpret_cast<const Vector *>(ent + kEntAbsOrigin) - t).Length();
        ++g_hold.logs;
        IVR_LOG("Hold in hand: hold point %.0f units from the eye (%.0f %.0f %.0f), object %.1f units from its "
                "target",
                rel.Length(), rel.x, rel.y, rel.z, off);
    }
    hkGrabSetTarget.original(self, edx, &t, &a, teleport);
    if (++g_hold.frames == 300)
    {
        g_hold.onTrial = false;
        flags::End(kHoldInHandFlag);
        IVR_LOG("Hold in hand: 300 updates without trouble");
    }
}

const QAngle *IVR_FASTCALL EyeAnglesDetour(void *self, void *edx)
{
    if (g_usePlayer && self == g_usePlayer)
        return &g_useAngles;
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
    if (g_Game.surface && flags::Begin(kPanelScaleFlag)) // ended after 60 scaled panels
    {
        void *fn = (*static_cast<void ***>(g_Game.surface))[vidx::DrawPanelIn3DSpace];
        hkDrawPanelIn3DSpace.Install("IMatSystemSurface::DrawPanelIn3DSpace", fn, (void *)&DrawPanelIn3DSpaceDetour);
    }
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
    if (g_Config.handUse && g_Game.fnClientFindUseEntity && g_Game.fnClientEyePosition && g_Game.fnClientEyeAngles &&
        flags::Begin(kHandUseFlag)) // ended after 600 outline searches from the hands
    {
        g_Mod.handUseClient =
            hkClientEyePosition.Install("C_INFRA_Player::EyePosition", g_Game.fnClientEyePosition,
                                        (void *)&ClientEyePositionDetour) &&
            hkClientEyeAngles.Install("C_INFRA_Player::EyeAngles", g_Game.fnClientEyeAngles,
                                      (void *)&ClientEyeAnglesDetour) &&
            hkClientFindUseEntity.Install("C_INFRA_Player::FindUseEntity", g_Game.fnClientFindUseEntity,
                                          (void *)&ClientFindUseEntityDetour);
    }
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
    if (g_Game.fnPlayerFindUseEntity && g_Game.fnPlayerEyePosition)
        g_Mod.handUseServer = hkEyePosition.Install("CINFRA_Player::EyePosition", g_Game.fnPlayerEyePosition,
                                                    (void *)&EyePositionDetour) &&
                              hkFindUseEntity.Install("CINFRA_Player::FindUseEntity", g_Game.fnPlayerFindUseEntity,
                                                      (void *)&FindUseEntityDetour);
    if (g_Game.fnGrabSetTarget && flags::Allowed(kHoldInHandFlag)) // on trial from the first held update to the 300th
        g_Mod.holdInHandReady = hkGrabSetTarget.Install("CGrabController::SetTargetPosition", g_Game.fnGrabSetTarget,
                                                        (void *)&GrabSetTargetDetour);
    if (g_Mod.holdInHandReady && g_Game.fnGrabDetach)
        hkGrabDetach.Install("CGrabController::DetachEntity", g_Game.fnGrabDetach, (void *)&GrabDetachDetour);
    IVR_LOG("Hand use: server %s, outline %s; hold in hand %s", g_Mod.handUseServer ? "on" : "off",
            g_Mod.handUseClient ? "on" : "off", g_Mod.holdInHandReady ? "on" : "off");
    return true;
}

} // namespace ivr

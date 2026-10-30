#include "world_size.h"
#include "flags.h"
#include "game.h"
#include "hooks.h"

namespace ivr {
namespace world {

namespace {

// CTexture::ApplyRenderTargetSizeMode(int &width, int &height, ImageFormat) in materialsystem.dll
// (INFRA 3.3.3: RVA 0x2e3b0). It starts by reading the requested size (ushorts at +0x34/+0x36),
// then switches on the size mode (+0x50, cases 1..6) and calls g_pMaterialSystem's
// GetBackBufferDimensions (vtable +0x84) for every mode that depends on the frame buffer.
// The function ends at its jump table, 0x138 bytes in.
const char *kApplySizeMode = "55 8B EC 0F B7 41 34 56 8B 75 08 89 06 0F B7 51 36 57 8B 7D 0C 89 17 8B 41 50 48 83 F8 05";
constexpr uint32_t kApplySizeModeLength = 0x138;

const char *kFlag = "world_eye_size";
constexpr int kTrustFrames = 600; // stereo frames before the crash-sticky flag is cleared

using GetDimsFn = void(IVR_FASTCALL *)(void *, void *, int *, int *);
Hook<GetDimsFn> g_hook;

uint8_t *g_sizeModeBegin = nullptr, *g_sizeModeEnd = nullptr;
volatile LONG g_w = 0, g_h = 0;
volatile LONG g_resized = 0;
volatile DWORD g_eyeThread = 0;
int g_stereoFrames = 0;
bool g_installed = false;

void IVR_FASTCALL GetBackBufferDimensionsDetour(void *self, void *edx, int *w, int *h)
{
    uint8_t *caller = static_cast<uint8_t *>(IVR_RETURN_ADDRESS());
    g_hook.original(self, edx, w, h);
    if (!w || !h)
        return;
    bool sizeMode = caller >= g_sizeModeBegin && caller < g_sizeModeEnd;
    bool eye = g_eyeThread && g_eyeThread == GetCurrentThreadId();
    if (!sizeMode && !eye)
        return;
    if (sizeMode)
    {
        LONG n = InterlockedIncrement(&g_resized);
        if (n <= 8)
            IVR_LOG("world_eye_size: render target sized from %dx%d to the eye size %ldx%ld", *w, *h, g_w, g_h);
    }
    *w = g_w;
    *h = g_h;
}

} // namespace

bool Install(void *materials, uint32_t eyeWidth, uint32_t eyeHeight)
{
    if (g_installed)
    {
        InterlockedExchange(&g_w, (LONG)eyeWidth);
        InterlockedExchange(&g_h, (LONG)eyeHeight);
        return true;
    }
    if (!materials || !eyeWidth || !eyeHeight)
        return false;
    if (!flags::Begin(kFlag))
    {
        IVR_LOG("world_eye_size is skipped - post-processing stays off in VR");
        return false;
    }
    int hits = 0;
    uint8_t *fn = FindPattern("materialsystem.dll", kApplySizeMode, &hits);
    if (!fn || hits != 1)
    {
        flags::Skip(kFlag, "CTexture::ApplyRenderTargetSizeMode not found in materialsystem.dll");
        return false;
    }
    g_sizeModeBegin = fn;
    g_sizeModeEnd = fn + kApplySizeModeLength;
    g_w = (LONG)eyeWidth;
    g_h = (LONG)eyeHeight;
    void *target = VTable(materials)[vidx::GetBackBufferDimensions];
    if (!g_hook.Install("CMaterialSystem::GetBackBufferDimensions", target,
                        reinterpret_cast<void *>(&GetBackBufferDimensionsDetour)))
    {
        flags::Skip(kFlag, "could not hook CMaterialSystem::GetBackBufferDimensions");
        return false;
    }
    g_installed = true;
    IVR_LOG("world_eye_size: full-frame render targets will be %ux%u (size-mode code at %p)", eyeWidth, eyeHeight,
            fn);
    return true;
}

bool Active() { return g_installed && g_resized > 0; }
int ResizedTargets() { return (int)g_resized; }

void BeginEye()
{
    if (g_installed)
        g_eyeThread = GetCurrentThreadId();
}

void EndEye() { g_eyeThread = 0; }

void OnStereoFrame()
{
    if (!g_installed || g_stereoFrames > kTrustFrames)
        return;
    if (++g_stereoFrames == kTrustFrames)
    {
        flags::End(kFlag);
        IVR_LOG("world_eye_size: %d render targets at the eye size, %d stereo frames without trouble",
                (int)g_resized, g_stereoFrames);
    }
}

} // namespace world
} // namespace ivr

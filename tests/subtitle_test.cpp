// Runs the subtitle panel pass (src/render/subtitle_shader.h) on known pixels and checks the
// alpha it rebuilds, plus the HudLayout region maths. Needs a D3D9 device (Windows, or Wine with a
// display such as xvfb-run).
#include "subtitle_shader.h"

#include <windows.h>
#include <cmath>
#include <cstdio>

using namespace ivr;

static int g_fail = 0;
static void Check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

int main()
{
    // HudCloseCaption in INFRA's HudLayout.res: xpos c-250, ypos 276, 500 x 136.
    float cc[4] = {-250, 276, 250, 412};
    RECT r = subtitles::Region(cc, 2560, 1440);
    Check(r.left == 530 && r.right == 2030 && r.top == 828 && r.bottom == 1236, "region at 2560x1440");
    float wide[4] = {-2000, -10, 2000, 900};
    r = subtitles::Region(wide, 1920, 1080);
    Check(r.left == 0 && r.right == 1920 && r.top == 0 && r.bottom == 1080, "region clamped to the texture");

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "subtest";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("subtest", "subtest", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, 0, 0, wc.hInstance, 0);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = TRUE;
    IDirect3DDevice9 *dev = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)))
    {
        printf("FAIL: no D3D9 device\n");
        return 1;
    }
    IDirect3DPixelShader9 *ps = nullptr;
    Check(SUCCEEDED(dev->CreatePixelShader(subtitles::kSubtitlePS, &ps)) && ps, "pixel shader accepted");
    if (!ps)
        return 1;

    // B G R A test pixels, as the game leaves them: painted onto transparent black.
    const UINT W = 6, H = 1;
    const BYTE in[W][4] = {
        {0, 0, 0, 0},         // nothing drawn
        {255, 255, 255, 0},   // white text, alpha lost (the bug)
        {255, 255, 255, 255}, // white text, alpha kept
        {128, 128, 128, 128}, // half-covered white edge (premultiplied)
        {0, 200, 255, 0},     // yellow-ish text, alpha lost
        {20, 20, 20, 0},      // faint edge
    };
    IDirect3DTexture9 *src = nullptr, *dst = nullptr, *sysTex = nullptr;
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &src, nullptr);
    dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, nullptr);
    dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysTex, nullptr);
    D3DLOCKED_RECT lr{};
    sysTex->LockRect(0, &lr, nullptr, 0);
    memcpy(lr.pBits, in, sizeof(in));
    sysTex->UnlockRect(0);
    dev->UpdateTexture(sysTex, src);

    auto run = [&](float gain, float bg, BYTE out[W][4]) {
        IDirect3DSurface9 *ds = nullptr, *sys = nullptr;
        dst->GetSurfaceLevel(0, &ds);
        bool ok = subtitles::DrawPanel(dev, src, ds, W, H, ps, gain, bg);
        dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        dev->GetRenderTargetData(ds, sys);
        D3DLOCKED_RECT l{};
        sys->LockRect(&l, nullptr, D3DLOCK_READONLY);
        memcpy(out, l.pBits, W * 4);
        sys->UnlockRect();
        sys->Release();
        ds->Release();
        for (UINT i = 0; i < W; ++i)
            printf("    in %3d %3d %3d a%3d -> out %3d %3d %3d a%3d\n", in[i][2], in[i][1], in[i][0], in[i][3],
                   out[i][2], out[i][1], out[i][0], out[i][3]);
        return ok;
    };
    auto approx = [](int a, int b) { return std::abs(a - b) <= 3; };
    BYTE o[W][4];
    printf("gain 1.5, background 0\n");
    Check(run(1.5f, 0.0f, o), "draw");
    Check(o[0][3] == 0, "nothing drawn stays transparent");
    Check(o[1][3] == 255 && o[1][2] == 255, "white text with lost alpha becomes solid white");
    Check(o[2][3] == 255 && o[2][0] == 255, "solid white stays solid white");
    Check(approx(o[3][3], 192) && approx(o[3][2], 170), "half-covered edge: alpha 0.75, colour unpremultiplied");
    Check(o[4][3] == 255 && approx(o[4][2], 255) && approx(o[4][1], 200) && approx(o[4][0], 0), "coloured text keeps its colour");
    Check(approx(o[5][3], 30) && approx(o[5][2], 170), "faint edge stays faint");
    printf("gain 1, background 0.5\n");
    Check(run(1.0f, 0.5f, o), "draw");
    Check(approx(o[0][3], 128) && o[0][2] == 0, "background: empty pixels are half-transparent black");
    Check(o[1][3] == 255 && o[1][2] == 255, "text over the background is solid");
    printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

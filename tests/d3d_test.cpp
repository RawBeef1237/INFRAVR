// End-to-end test of the D3D9 -> DXVK -> OpenVR path (run under Wine + lavapipe with the fake
// openvr_api.dll): eye/HUD textures are captured, transitioned, submitted and read back.
#include "common.h"
#include "d3d_vr.h"
#include "hooks.h"
#include "vr.h"
#include <cstdio>
#include <algorithm>
#include <d3d9.h>

using namespace ivr;
extern "C" IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk);

static void Fill(IDirect3DDevice9 *dev, IDirect3DTexture9 *t, D3DCOLOR c)
{
    IDirect3DSurface9 *s = nullptr;
    t->GetSurfaceLevel(0, &s);
    dev->ColorFill(s, nullptr, c);
    s->Release();
}

int main()
{
    LogInit("d3d_test_infravr.log");
    HooksInit();
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "Valve001";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("Valve001", "INFRA", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 816, 639, nullptr, nullptr,
                             wc.hInstance, nullptr);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
    {
        printf("Direct3DCreate9 failed\n");
        return 1;
    }
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = 800;
    pp.BackBufferHeight = 600;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = TRUE;
    pp.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES; // like the log: MSAA 4
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    printf("CreateDevice %08lx active %d interop %d\n", (unsigned long)hr, g_Mod.Active(), g_D3D.interopOk);
    if (FAILED(hr))
        return 1;

    UINT w = g_VR.renderWidth, h = g_VR.renderHeight;
    IDirect3DTexture9 *eyes[2] = {}, *hud = nullptr;
    for (int e = 0; e < 2; ++e)
    {
        g_D3D.capturing = e; // what the mod does around CreateNamedRenderTargetTextureEx
        dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &eyes[e], nullptr);
    }
    g_D3D.capturing = 2;
    dev->CreateTexture(800, 600, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &hud, nullptr);
    g_D3D.capturing = -1;
    printf("captured L %d R %d HUD %d (%ux%u)\n", g_D3D.eye[0].Valid(), g_D3D.eye[1].Valid(), g_D3D.hud.Valid(), w, h);

    for (int frame = 0; frame < 20; ++frame)
    {
        bool stereo = frame < 16;
        Fill(dev, eyes[0], D3DCOLOR_XRGB(255, 0, 0));
        {
            // left eye: a 800x500 picture whose last row/column is smeared over the rest
            IDirect3DSurface9 *s = nullptr;
            eyes[0]->GetSurfaceLevel(0, &s);
            for (UINT y = 0; y < h; y += 10)
                for (UINT x = 0; x < w; x += 10)
                {
                    UINT cx = x < 800 ? x : 790, cy = y < 500 ? y : 490;
                    RECT r = {(LONG)x, (LONG)y, (LONG)std::min(w, x + 10), (LONG)std::min(h, y + 10)};
                    dev->ColorFill(s, &r, D3DCOLOR_XRGB((cx * 7 + cy * 3) & 255, (cx + cy * 5) & 255, 64));
                }
            s->Release();
        }
        if (frame == 10)
        {
            // the engine replaces the left eye texture on a level change: the mod must follow it
            dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &eyes[0], nullptr);
            IDirect3DSurface9 *s = nullptr, *old = nullptr;
            dev->GetRenderTarget(0, &old);
            eyes[0]->GetSurfaceLevel(0, &s);
            dev->SetRenderTarget(0, s);
            g_D3D.CheckBoundRenderTarget(0);
            dev->SetRenderTarget(0, old);
            s->Release();
            old->Release();
            printf("left eye texture replaced, mod follows: %d\n", g_D3D.eye[0].d3d == eyes[0]);
        }
        if (frame >= 10)
            Fill(dev, eyes[0], D3DCOLOR_XRGB(255, 255, 255));
        Fill(dev, eyes[1], D3DCOLOR_XRGB(0, 255, 0));
        Fill(dev, hud, D3DCOLOR_ARGB(128, 0, 0, 255));
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0x40, 255, 255, 0), 1.0f, 0); // engine-like junk alpha
        g_Mod.menuMode = !stereo;
        g_Mod.eyesRendered = stereo;
        g_Mod.hudPainted = stereo;
        hr = dev->Present(nullptr, nullptr, nullptr, nullptr);
        Sleep(1600);
        printf("frame %d (%s) present %08lx\n", frame, stereo ? "stereo" : "menu", (unsigned long)hr);
    }
    // Reset path
    g_D3D.OnBeforeReset();
    printf("done\n");
    return 0;
}

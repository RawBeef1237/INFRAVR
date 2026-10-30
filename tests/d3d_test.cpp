// End-to-end test of the D3D9 -> DXVK -> OpenVR path (run under Wine + lavapipe with the fake
// openvr_api.dll): eye/HUD textures are captured, transitioned, submitted and read back.
#include "common.h"
#include "d3d_vr.h"
#include "hooks.h"
#include "mod.h"
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
    g_Mod.firstLevelAt = NowSeconds() - 5.0; // as if the main menu map had loaded
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
    // Antialiasing: drawing into an eye goes to a multisampled stand-in that is resolved into it.
    {
        IDirect3DSurface9 *eyeSurf = nullptr, *depth = nullptr, *old = nullptr, *oldDepth = nullptr;
        eyes[1]->GetSurfaceLevel(0, &eyeSurf);
        dev->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth, nullptr);
        dev->GetRenderTarget(0, &old);
        dev->GetDepthStencilSurface(&oldDepth);
        dev->SetRenderTarget(0, eyeSurf);
        dev->SetDepthStencilSurface(depth);
        IDirect3DSurface9 *bound = nullptr;
        dev->GetRenderTarget(0, &bound);
        D3DSURFACE_DESC bd{};
        bound->GetDesc(&bd);
        HRESULT ch = dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(255, 0, 255), 1.0f, 0);
        // a copy the game makes from the eye while drawing it (like _rt_FullFrameFB)
        IDirect3DTexture9 *copyTex = nullptr;
        IDirect3DSurface9 *copySurf = nullptr;
        dev->CreateTexture(512, 512, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &copyTex, nullptr);
        copyTex->GetSurfaceLevel(0, &copySurf);
        HRESULT cp = dev->StretchRect(eyeSurf, nullptr, copySurf, nullptr, D3DTEXF_LINEAR);
        dev->SetRenderTarget(0, old);
        dev->SetDepthStencilSurface(oldDepth);
        g_D3D.ResolveEyes();
        auto pixel = [&](IDirect3DSurface9 *src, UINT sw, UINT sh) -> DWORD {
            IDirect3DSurface9 *sys = nullptr;
            dev->CreateOffscreenPlainSurface(sw, sh, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
            dev->GetRenderTargetData(src, sys);
            D3DLOCKED_RECT lr{};
            DWORD v = 0;
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
            {
                v = static_cast<DWORD *>(lr.pBits)[(sh / 2) * (lr.Pitch / 4) + sw / 2] & 0xFFFFFF;
                sys->UnlockRect();
            }
            sys->Release();
            return v;
        };
        DWORD eyePx = pixel(eyeSurf, w, h), copyPx = pixel(copySurf, 512, 512);
        printf("msaa: bound samples %d, clear %08lx, copy %08lx, eye pixel %06lx, copy pixel %06lx -> %s\n",
               (int)bd.MultiSampleType, (unsigned long)ch, (unsigned long)cp, (unsigned long)eyePx,
               (unsigned long)copyPx,
               bd.MultiSampleType == D3DMULTISAMPLE_4_SAMPLES && eyePx == 0xFF00FF && copyPx == 0xFF00FF ? "OK" : "FAIL");
        bound->Release();
        copySurf->Release();
        copyTex->Release();
        old->Release();
        oldDepth->Release();
        depth->Release();
        eyeSurf->Release();
    }
    // Reset path
    g_D3D.OnBeforeReset();
    printf("done\n");
    return 0;
}

// Loads the built d3d9.dll exactly like shaderapidx9.dll would and presents a few frames.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
int main()
{
    HMODULE m = LoadLibraryExA("Z:\\home\\claude\\infravr3\\build\\d3d9.dll", nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto create = (IDirect3D9 * (WINAPI *)(UINT)) GetProcAddress(m, "Direct3DCreate9");
    printf("module %p Direct3DCreate9 %p\n", (void *)m, (void *)create);
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "Valve001";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("Valve001", "INFRA", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 656, 519, 0, 0, wc.hInstance, 0);
    IDirect3D9 *d3d = create(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = wnd;
    pp.Windowed = TRUE;
    pp.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    printf("CreateDevice %08lx\n", (unsigned long)hr);
    for (int i = 0; i < 4 && dev; ++i)
    {
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 1, 0);
        printf("present %08lx\n", (unsigned long)dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    return 0;
}

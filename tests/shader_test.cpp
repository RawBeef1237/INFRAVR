// Compiles the hand/camera shaders the way the mod does (D3DCompile from d3dcompiler_47, 43 or
// INFRA's own d3dcompiler_41). Usage: shader_test [path to a d3dcompiler dll]
#include "hand_shaders.h"

#include <windows.h>
#include <unknwn.h>
#include <cstdio>
#include <cstring>

struct IBlob : public IUnknown
{
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
using D3DCompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT,
                                       IBlob **, IBlob **);

static bool Try(D3DCompileFn compile, const char *src, const char *target)
{
    IBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = compile(src, strlen(src), "infravr_hands", nullptr, nullptr, "main", target, 1u << 15, 0, &code,
                         &errors);
    if (errors)
    {
        printf("  %s messages: %.*s\n", target, (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
        errors->Release();
    }
    if (FAILED(hr) || !code)
    {
        printf("  %s: FAIL 0x%08lx\n", target, (unsigned long)hr);
        return false;
    }
    const DWORD *tok = (const DWORD *)code->GetBufferPointer();
    printf("  %s: %u bytes, version token 0x%08lx\n", target, (unsigned)code->GetBufferSize(), (unsigned long)tok[0]);
    bool ok = (tok[0] & 0xFFFF0000u) == (target[0] == 'v' ? 0xFFFE0000u : 0xFFFF0000u) && (tok[0] & 0xFF00) == 0x0300;
    code->Release();
    return ok;
}

int main(int argc, char **argv)
{
    const char *dll = argc > 1 ? argv[1] : "d3dcompiler_47.dll";
    HMODULE m = LoadLibraryA(dll);
    auto compile = m ? (D3DCompileFn)(void *)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile)
    {
        printf("%s: no D3DCompile\nFAIL\n", dll);
        return 1;
    }
    printf("%s\n", dll);
    bool ok = Try(compile, ivr::hands::kVertexShader, "vs_3_0") && Try(compile, ivr::hands::kPixelShader, "ps_3_0");
    printf(ok ? "OK\n" : "FAIL\n");
    return ok ? 0 : 1;
}

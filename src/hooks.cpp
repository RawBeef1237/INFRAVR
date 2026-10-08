#include "hooks.h"
#include "vfunc.h"

#include "MinHook.h"

#include <psapi.h>
#include <cstdio>
#include <cstdlib>

namespace ivr {

static bool g_mhInit = false;

bool HooksInit()
{
    if (g_mhInit)
        return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
    {
        IVR_ERR("MH_Initialize failed: %d", (int)s);
        return false;
    }
    g_mhInit = true;
    return true;
}

bool InstallHook(const char *name, void *target, void *detour, void **original)
{
    if (!HooksInit())
        return false;
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK)
    {
        IVR_ERR("Hook %s: MH_CreateHook(%p) failed: %s", name, target, MH_StatusToString(s));
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK)
    {
        IVR_ERR("Hook %s: MH_EnableHook(%p) failed: %s", name, target, MH_StatusToString(s));
        MH_RemoveHook(target);
        return false;
    }
    IVR_LOG("Hooked %s at %p", name, target);
    return true;
}

ModuleInfo GetModuleInfo(const char *moduleName)
{
    ModuleInfo info;
    HMODULE mod = GetModuleHandleA(moduleName);
    if (!mod)
        return info;
    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
    {
        info.base = static_cast<uint8_t *>(mi.lpBaseOfDll);
        info.size = mi.SizeOfImage;
    }
    return info;
}

void *GetInterface(const char *moduleName, const char *interfaceName)
{
    HMODULE mod = GetModuleHandleA(moduleName);
    if (!mod)
        return nullptr;
    auto factory = reinterpret_cast<void *(__cdecl *)(const char *, int *)>(GetProcAddress(mod, "CreateInterface"));
    if (!factory)
        return nullptr;
    int rc = 0;
    return factory(interfaceName, &rc);
}

static bool ParsePattern(const char *pattern, std::vector<int> &out)
{
    out.clear();
    const char *p = pattern;
    while (*p)
    {
        while (*p == ' ')
            ++p;
        if (!*p)
            break;
        if (*p == '?')
        {
            out.push_back(-1);
            ++p;
            if (*p == '?')
                ++p;
        }
        else
        {
            char *end = nullptr;
            long v = strtol(p, &end, 16);
            if (end == p)
                return false;
            out.push_back((int)v);
            p = end;
        }
    }
    return !out.empty();
}

static bool MatchAt(const uint8_t *p, const std::vector<int> &bytes)
{
    for (size_t j = 0; j < bytes.size(); ++j)
        if (bytes[j] >= 0 && p[j] != (uint8_t)bytes[j])
            return false;
    return true;
}

// Code section range of a loaded module.
static bool CodeRange(const ModuleInfo &mi, uint8_t *&begin, uint8_t *&end)
{
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(mi.base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(mi.base + dos->e_lfanew);
    auto *sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)
        {
            begin = mi.base + sec->VirtualAddress;
            end = begin + sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

uint8_t *FindPattern(const char *moduleName, const char *pattern, int *hits)
{
    if (hits)
        *hits = 0;
    ModuleInfo mi = GetModuleInfo(moduleName);
    if (!mi.base)
        return nullptr;
    std::vector<int> bytes;
    if (!ParsePattern(pattern, bytes))
        return nullptr;
    uint8_t *b = nullptr, *e = nullptr;
    if (!CodeRange(mi, b, e))
        return nullptr;
    uint8_t *first = nullptr;
    for (uint8_t *p = b; p + bytes.size() <= e; ++p)
    {
        if (MatchAt(p, bytes))
        {
            if (!first)
                first = p;
            if (hits)
                ++*hits;
            else
                break;
        }
    }
    return first;
}

void *ResolveFunction(const char *what, const char *moduleName, uint32_t rva, const char *pattern)
{
    ModuleInfo mi = GetModuleInfo(moduleName);
    if (!mi.base)
    {
        IVR_ERR("%s: module %s not loaded", what, moduleName);
        return nullptr;
    }
    std::vector<int> bytes;
    ParsePattern(pattern, bytes);
    if (rva && rva + bytes.size() < mi.size && MatchAt(mi.base + rva, bytes))
        return mi.base + rva;
    int hits = 0;
    uint8_t *p = FindPattern(moduleName, pattern, &hits);
    if (hits == 1)
    {
        IVR_LOG("%s: found by signature at %s+0x%X (expected +0x%X)", what, moduleName, (unsigned)(p - mi.base),
                rva);
        return p;
    }
    IVR_ERR("%s: not found in %s (%d signature matches)", what, moduleName, hits);
    return nullptr;
}

bool IsReadable(const void *address, size_t bytes)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi)))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<uintptr_t>(address) + bytes <= end;
}

static bool IsExecutable(const void *address)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!address || !VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

bool LooksLikeObject(const void *object)
{
    if (!IsReadable(object, sizeof(void *)))
        return false;
    void **vt = VTable(object);
    if (!IsReadable(vt - 1, 2 * sizeof(void *)))
        return false;
    return IsExecutable(vt[0]);
}

// RTTI structures (x86): CompleteObjectLocator { sig, offset, cdOffset, TypeDescriptor*, ClassHierarchy* }
// TypeDescriptor { pVFTable, spare, char name[] }
void **FindVTable(const char *moduleName, const char *mangled)
{
    ModuleInfo mi = GetModuleInfo(moduleName);
    if (!mi.base)
        return nullptr;
    size_t nameLen = strlen(mangled) + 1;
    uint8_t *end = mi.base + mi.size;
    // 1) type descriptor: name follows two pointers
    uint8_t *td = nullptr;
    for (uint8_t *p = mi.base + 8; p + nameLen < end; ++p)
    {
        if (p[0] == '.' && p[1] == '?' && memcmp(p, mangled, nameLen) == 0)
        {
            td = p - 8;
            break;
        }
    }
    if (!td)
        return nullptr;
    // 2) complete object locator with offset 0 referencing it
    for (uint8_t *p = mi.base; p + 20 <= end; p += 4)
    {
        auto *col = reinterpret_cast<uint32_t *>(p);
        if (col[0] != 0 || col[1] != 0 || col[3] != reinterpret_cast<uint32_t>(td))
            continue;
        // 3) vtable: the word after a pointer to this COL
        uint32_t colAddr = reinterpret_cast<uint32_t>(p);
        for (uint8_t *q = mi.base; q + 8 <= end; q += 4)
        {
            if (*reinterpret_cast<uint32_t *>(q) == colAddr)
            {
                void **vt = reinterpret_cast<void **>(q + 4);
                if (IsExecutable(vt[0]))
                    return vt;
            }
        }
    }
    return nullptr;
}

const char *RttiName(const void *object)
{
    if (!LooksLikeObject(object))
        return "";
    void **vt = VTable(object);
    auto *col = static_cast<uint32_t *>(vt[-1]);
    if (!IsReadable(col, 20))
        return "";
    auto *td = reinterpret_cast<uint8_t *>(col[3]);
    if (!IsReadable(td, 16))
        return "";
    return reinterpret_cast<const char *>(td + 8);
}

} // namespace ivr

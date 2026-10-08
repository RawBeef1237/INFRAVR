// INFRA VR - hooking helpers (MinHook), module scanning, RTTI lookups.
#pragma once
#include "common.h"

namespace ivr {

bool HooksInit();
bool InstallHook(const char *name, void *target, void *detour, void **original);

template <typename Fn>
struct Hook
{
    Fn original = nullptr;
    void *target = nullptr;

    bool Install(const char *name, void *targetFn, void *detour)
    {
        if (!targetFn)
        {
            IVR_ERR("Hook %s: target not found", name);
            return false;
        }
        if (!InstallHook(name, targetFn, detour, reinterpret_cast<void **>(&original)))
            return false;
        target = targetFn;
        return true;
    }
    bool Installed() const { return target != nullptr; }
};

struct ModuleInfo
{
    uint8_t *base = nullptr;
    size_t size = 0;
};

ModuleInfo GetModuleInfo(const char *moduleName);
void *GetInterface(const char *moduleName, const char *interfaceName);

// IDA-style pattern ("55 8B EC ? ? 8B") scan inside a module's code.
uint8_t *FindPattern(const char *moduleName, const char *pattern, int *hits = nullptr);

// Returns module+rva if the pattern matches there, otherwise scans the module for a
// unique match. `what` is only used for logging.
void *ResolveFunction(const char *what, const char *moduleName, uint32_t rva, const char *pattern);

// MSVC RTTI: vtable of class `mangled` (".?AVC_INFRA_Player@@") at complete-object offset 0.
void **FindVTable(const char *moduleName, const char *mangled);
// RTTI name of a polymorphic object (".?AVC_INFRACamera@@"), or "" if unavailable.
const char *RttiName(const void *object);

bool IsReadable(const void *address, size_t bytes);
bool LooksLikeObject(const void *object);

} // namespace ivr

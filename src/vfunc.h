// INFRA VR - calling engine virtual functions by index (MSVC x86 __thiscall).
#pragma once
#include "common.h"

namespace ivr {

inline void **VTable(const void *object) { return *reinterpret_cast<void **const *>(object); }

template <typename Ret, typename... Args>
inline Ret VCall(const void *object, int index, Args... args)
{
    using Fn = Ret(IVR_THISCALL *)(const void *, Args...);
    return reinterpret_cast<Fn>(VTable(object)[index])(object, args...);
}

template <typename Ret, typename... Args>
inline Ret ThisCall(const void *fn, const void *object, Args... args)
{
    using Fn = Ret(IVR_THISCALL *)(const void *, Args...);
    return reinterpret_cast<Fn>(const_cast<void *>(fn))(object, args...);
}

template <typename T>
inline T &Field(void *base, size_t offset)
{
    return *reinterpret_cast<T *>(static_cast<uint8_t *>(base) + offset);
}

} // namespace ivr

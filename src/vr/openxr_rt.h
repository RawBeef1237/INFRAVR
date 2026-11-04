// INFRA VR - game side of the OpenXR helper (see openxr_bridge.h).
#pragma once
#include "common.h"
#include "openxr_bridge.h"

namespace ivr {
namespace xr {

struct Link
{
    HANDLE map = nullptr;
    HANDLE frameEvent = nullptr;
    HANDLE process = nullptr;
    ivrxr::Shared *shm = nullptr;
    uint32_t lastPoseSeq = 0;
    bool Alive() const; // helper still running and not failed
};

// Creates the shared memory, starts infravr\bin\infravr_openxr.exe and waits until it
// reports the headset (or fails / times out). nullptr on failure, with the reason logged.
Link *Start(double timeoutSeconds);
void Stop(Link *link);

} // namespace xr
} // namespace ivr

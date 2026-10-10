// INFRA VR - the VR controllers as an Xbox controller for INFRA's own gamepad support.
#pragma once
#include "common.h"

namespace ivr {

// Hooks XInputGetState / XInputGetCapabilities in XInput1_3.dll (what INFRA's inputsystem uses).
// Call before the engine's input system starts, so it sees controller 1 as connected.
void InstallGamepadHooks();

} // namespace ivr

// INFRA VR - shared memory between the game (32-bit d3d9.dll) and infravr_openxr.exe (64-bit
// OpenXR helper). The same idea as Black Mesa VR's OpenXRHelper64: OpenXR runtimes are 64-bit
// only, INFRA is 32-bit, so a small helper process owns the OpenXR session.
//
//   helper -> game : runtime info, view FOV, head/hand poses, controller input
//   game -> helper : eye images and overlay panels as shared D3D11 textures (KMT handles), their
//                    poses, haptic pulses
//
// Both sides are plain C structs with fixed sizes so the 32- and 64-bit builds agree.
// Coordinates are OpenVR/OpenXR conventions (metres, right-handed, Y up, -Z forward), poses as
// row-major 3x4 matrices in the standing (stage) space.
#pragma once
#include <stdint.h>

namespace ivrxr {

constexpr uint32_t kMagic = 0x52585649; // "IVXR"
constexpr uint32_t kVersion = 1;
constexpr int kLayers = 4;              // menu screen, HUD/phone panel, pointer dot, pointer beam
constexpr uint32_t kRawBytes = 64 * 1024; // pixels of a small overlay (RGBA8)
constexpr int kEyeSlots = 3;              // eye image pairs in flight

// Shared memory and event names get the game's process id appended: "<name><pid>".
#define IVRXR_SHM_NAME "Local\\INFRAVR_OpenXR_"
#define IVRXR_FRAME_EVENT "Local\\INFRAVR_OpenXR_Frame_" // helper -> game: new poses
#define IVRXR_SUBMIT_EVENT "Local\\INFRAVR_OpenXR_Submit_" // game -> helper: new eye images

enum Button : uint32_t
{
    BTN_JUMP = 1u << 0,
    BTN_CROUCH = 1u << 1,
    BTN_SPRINT = 1u << 2,
    BTN_USE_RIGHT = 1u << 3,
    BTN_USE_LEFT = 1u << 4,
    BTN_GRIP_LEFT = 1u << 5,
    BTN_GRIP_RIGHT = 1u << 6,
    BTN_FLASHLIGHT = 1u << 7,
    BTN_PHONE = 1u << 8,
    BTN_RELOAD = 1u << 9,
    BTN_PAUSE = 1u << 10,
    BTN_RECENTER = 1u << 11,
};

struct alignas(8) Pose
{
    float m[3][4];
    uint32_t valid;
    float velocity[3]; // m/s
};

enum LayerKind : uint32_t
{
    LAYER_NONE = 0,
    LAYER_SHARED = 1, // sharedHandle is a D3D shared texture (KMT handle)
    LAYER_RAW = 2,    // raw[] holds width*height RGBA8 pixels
};

struct alignas(8) Layer
{
    uint32_t visible;
    uint32_t kind;
    uint32_t headLocked; // xf is relative to the head, not the standing space
    uint32_t sortOrder;
    uint64_t sharedHandle;
    uint32_t width, height;
    float widthMeters;
    float xf[3][4];
    uint32_t serial; // changes whenever the texture handle or raw pixels change
};

struct alignas(8) Shared
{
    uint32_t magic, version;
    uint32_t gamePid, helperPid;

    // ---- helper -> game -------------------------------------------------------------------
    volatile uint32_t helperState; // 0 starting, 1 ready (fields below valid), 2 failed, 3 quit
    char runtimeName[128];
    char error[256];
    uint32_t recommendedWidth, recommendedHeight;
    uint32_t adapterLuidLow;
    int32_t adapterLuidHigh;
    float fov[2][4];      // per eye: tan(left) (negative), tan(right), tan(up), tan(down) (negative)
    float eyeToHead[2][3]; // eye position relative to the head, metres
    volatile uint32_t poseSeq; // odd while being written
    Pose head;
    Pose hands[2]; // aim poses (like OpenVR /pose/tip)
    float move[2], turn[2];
    uint32_t buttons;
    float trigger[2], squeeze[2];
    uint32_t sessionFocused;

    // ---- game -> helper -------------------------------------------------------------------
    volatile uint32_t eyeSeq; // published eye slot sequence number (slot = eyeSeq % kEyeSlots)
    uint64_t eyeHandle[kEyeSlots][2];
    uint32_t eyeWidth, eyeHeight;
    float renderFov[4]; // the symmetric frustum the game drew: tan left, right, up, down
    Pose renderHead[kEyeSlots]; // head pose each slot was drawn for
    volatile uint32_t helperUsingSlot; // slot the helper is copying from (0xFFFFFFFF = none)
    Layer layers[kLayers];
    uint8_t raw[kLayers][kRawBytes];
    volatile uint32_t hapticSeq[2];
    float hapticSeconds[2], hapticFrequency[2], hapticAmplitude[2];
    volatile uint32_t gameQuit;
    volatile uint32_t eyesVisible; // 0 while the game shows a flat menu: no projection layer
    uint32_t perEyeFov;            // 1: each eye was drawn with renderFovEye[eye], not renderFov
    float renderFovEye[2][4];      // per eye: tan left, right, up, down
};

} // namespace ivrxr

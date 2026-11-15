// INFRA VR - CPU core usage, written to the log so you can tell whether multicore rendering
// (mat_queue_mode 2) really spreads the work over more than one core.
//
// Every few seconds Report() measures how much CPU time each of INFRA's threads used since the
// last report (GetThreadTimes) and writes one line: the process total in "cores' worth" of work,
// the threads that kept a core busy, the engine's main thread and the render thread that calls
// Present, and which processors those two actually ran on (sampled with GetCurrentProcessorNumber).
#pragma once
#include "common.h"

namespace ivr {
namespace cpu {

// The thread roles whose processor is sampled.
enum Role
{
    MainThread = 0,   // the engine's thread (CreateMove, RenderView)
    RenderThread = 1, // the thread that calls Present (the material system's thread with multicore)
    RoleCount
};

// Remembers which processor the calling thread is on right now (cheap; call once per frame).
void NoteProcessor(Role role);

// Writes a report if the interval has passed. Call from the engine's main thread once per frame.
// queueMode is the mat_queue_mode in effect (what the mod asked for), multicoreWanted whether the
// game's own "multicore rendering" option is on; mainTid / renderTid are the two thread ids;
// queued whether the material system really hands out a queued render context (its call queue).
void Report(int queueMode, bool multicoreWanted, bool queued, DWORD mainTid, DWORD renderTid);

// Starts a fresh measurement (e.g. when mat_queue_mode changes), so a report covers one mode only.
void Restart();

} // namespace cpu
} // namespace ivr

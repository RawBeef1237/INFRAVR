// INFRA VR - crash-sticky experiment flags (the approach Black Mesa VR uses).
//
// A risky mechanism calls Begin("name") before it first runs and End("name") once it has run
// long enough to be trusted. Begin writes INFRA\infravr\infravr_in_<name>.flag; End deletes it. If the
// game dies in between, the flag is still there on the next launch: that mechanism is added to
// INFRA\infravr\infravr_skip.txt and stays off until the user deletes the line (or sets
// [debug] retry_skipped = true). Everything else keeps working.
#pragma once
#include "common.h"

namespace ivr {
namespace flags {

// Reads infravr_skip.txt, turns leftover infravr_in_*.flag files into skip lines.
void Init();

// True if `name` is allowed to run this launch (not in the skip file).
bool Allowed(const char *name);
// Marks `name` as in progress. Returns false (and does nothing) if it is skipped.
bool Begin(const char *name);
// The mechanism survived: removes its in-progress flag.
void End(const char *name);
// Turns `name` off for future launches without a crash (e.g. a check failed), with a reason.
void Skip(const char *name, const char *reason);

// What the mod is doing right now, written to the log if INFRA crashes.
void SetStage(const char *stage);
const char *Stage();

// Logs the stage and exception if INFRA crashes (chains to the previous handler). Called again
// once the engine is up, so it runs before the engine's own crash handler.
void InstallCrashLogger();
// INFRA is exiting normally (not after a crash our filter saw): mechanisms still on trial are not
// held against the next launch.
void OnProcessExit();
// Treats the engine's own TerminateProcess on quit as a normal exit too.
void InstallExitHook();

} // namespace flags
} // namespace ivr

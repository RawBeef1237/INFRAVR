// INFRA VR - crash-sticky experiment flags (the approach Black Mesa VR uses).
//
// A risky mechanism calls Begin("name") before it first runs and End("name") once it has run
// long enough to be trusted. Begin writes INFRA\infravr_in_<name>.flag; End deletes it. If the
// game dies in between, the flag is still there on the next launch: that mechanism is added to
// INFRA\infravr_skip.txt and stays off until the user deletes the line (or sets
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

// INFRA is quitting normally (DLL_PROCESS_DETACH at process exit): mechanisms still in their
// trial run did not crash, so their in-progress flags are removed instead of becoming skips.
void OnCleanExit();

// What the mod is doing right now, written to the log if INFRA crashes.
void SetStage(const char *stage);
const char *Stage();

// Logs the stage and exception if INFRA crashes (chains to the previous handler).
void InstallCrashLogger();

} // namespace flags
} // namespace ivr

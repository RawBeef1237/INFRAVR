# Working on INFRA VR

Rules for anyone (people or coding agents) changing this repository.

- **No version numbers.** The maintainer numbers releases. Do not write version numbers into code,
  docs, the ini or commit messages; the build takes `-DINFRAVR_VERSION=...` and defaults to `dev`.
- **Evidence first.** Every offset, vtable slot or signature goes into `docs/OFFSETS.md` with how
  it was found and its status grade. Failed approaches stay there as **verified-fail**.
- **Find, don't hard-code.** Functions are found by RTTI vtable + slot or by a unique signature;
  a signature must match exactly once or the feature turns itself off.
- **Risky mechanisms are crash-sticky.** Wrap anything that can crash INFRA in
  `flags::Begin("name")` / `flags::End("name")` and give it a way to fall back.
- **No console commands from Present or CreateMove's hot path.** Queue them with
  `VRMod::QueueCommand`; they run at the RenderView safe point.
- **Stereo once per Present.** Do not add code paths that draw the eyes twice in a frame.
- **Respect the module boundaries** in `docs/ARCHITECTURE.md`; the OpenXR helper shares only
  `src/vr/openxr_bridge.h` with the mod, and that struct is append-only.
- **Test what can be tested.** `-DINFRAVR_TESTS=ON` builds `glb_test` and `resolve_test`; both run
  under Wine. Update `docs/RUNTIME.md` with what was and was not run.

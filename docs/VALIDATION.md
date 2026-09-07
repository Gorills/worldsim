# Validation record

This file distinguishes what was actually executed from architectural intent.

## Core test coverage

`worldsim_tests` covers:

- scheduler cadence/elapsed-time alignment;
- cube-sphere global area closure;
- same-level cross-face neighbor validity;
- extensive-field conservation on refine/coarsen;
- intensive-field preservation/aggregation;
- cohort population conservation;
- deterministic same-seed/same-input snapshot equality within the tested build;
- snapshot continuation including future commands and pending events;
- adaptive LOD stability;
- snapshot restore from a different current LOD cover;
- command routing when the addressed coarse cell has been refined;
- columnar field-store consistency;
- cohort spatial index consistency;
- non-negative/finiteness ecology invariants;
- C ABI field discovery and value copying;
- module-extension contract and generic snapshot support;
- post-load spatial-store/active-cover validation through normal snapshot round trips.

`worldsim_sanitizer_smoke` is a bounded scenario intended for ASan/UBSan builds.

## Claims deliberately not made

- scientific calibration of the demo climate/ecology equations;
- bit-exact floating point identity across compilers/CPUs;
- production performance at the final target world scale;
- completed economy/politics/war/epidemic models;
- platform builds that were not actually executed during packaging.

## Packaging validation (2026-09-07)

Actually executed in the packaging environment:

- GCC 14.2.0, CMake 3.31.6, Ninja 1.12.1: clean configure/build and full `ctest`;
- Clang 17.0.0: clean configure/build, full `ctest`, and zero compiler warning/error diagnostics under the project warning flags;
- GCC ASan + UBSan bounded smoke with `halt_on_error=1`: passed;
- headless CLI: 720 one-hour ticks completed;
- final ZIP is re-extracted into a clean directory and rebuilt/tested before delivery.

## Godot 4.7 integration validation

GitHub Actions run `34122677420` on commit `4f00d53a31fd8488062130b9db7098e28c5d5db2` completed successfully on Ubuntu 22.04.

Actually verified there:

- `godot-cpp` 10.0.0-rc2 configured for Godot API 4.7;
- the `worldsim_godot` GDExtension compiled and linked as `libworldsim_godot.so`;
- the pinned official Godot 4.7.2 Linux editor archive passed SHA-256 verification;
- Godot reported `4.7.2.stable.official.ed1daf0bf`;
- `ci_smoke.gd` loaded the GDExtension, instantiated `WorldSimulationNode`, advanced 24 simulation ticks, and validated a non-empty render packet and field descriptors;
- smoke marker: `WORLDSIM_GODOT_SMOKE_OK tick=24 cells=1536 fields=13`;
- repeat GDExtension build used `sccache` with 71/71 cache hits (100%).

The standalone headless editor `--import` pass is deliberately best-effort. `godot-cpp` upstream CI documents the same editor-import abort behavior and ignores that process result before running its actual tests. WorldSim therefore uses the runtime `ci_smoke.gd` execution as the mandatory integration gate rather than treating editor-import teardown as a product failure.

## Architecture/Godot audit validation — 2026-09-07

Pull-request CI run `34125144404` on audit commit
`cf207e63226a7ed5100b77d5021b63b33609b33a` completed successfully on Ubuntu 22.04.

Actually verified in that run:

- Core/GCC CMake configure, build, and CTest completed successfully, including the new simulation-LOD hysteresis regression.
- The Godot 4.7 GDExtension configured and linked successfully against the pinned API/dependency setup.
- Godot reported `4.7.2.stable.official.ed1daf0bf`.
- `ci_smoke.gd` advanced the simulation to tick 24, returned 1536 active cells and 13 fields, and verified dense generic-field/render-packet alignment.
- The actual `res://main.tscn` then launched headlessly with `--language ru` and exited successfully after two iterations, exercising the viewer GDScript, project input map, Theme resource, and registered gettext resources at runtime.

The standalone editor `--import` process still aborts during teardown in this environment and remains intentionally best-effort, as documented above; the mandatory runtime smoke and actual scene launch both pass.

# Validation record

This file is updated by the final packaging pass. It distinguishes what was actually executed from architectural intent.

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

Godot 4.7 GDExtension source/CMake contracts were checked against Godot 4.7 documentation and `godot-cpp` 10.0.0-rc2 (release commit `5ed72a0dc2517a8082598a950895c6b24e8aa282`). The packaging environment has neither a Godot 4.7 editor/runtime nor a locally available `godot-cpp` tree, and outbound dependency fetching is unavailable there. Therefore actual GDExtension compilation/editor launch is **not verified** by this record.

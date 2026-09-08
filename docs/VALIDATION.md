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
- rejection of stale authoritative geography/geology snapshot epochs (versions 2 through 9);
- adaptive LOD stability;
- snapshot restore from a different current LOD cover;
- command routing when the addressed coarse cell has been refined;
- columnar field-store consistency;
- cohort spatial index consistency;
- non-negative/finiteness ecology invariants;
- C ABI field discovery and value copying;
- module-extension contract and generic snapshot support;
- post-load spatial-store/active-cover validation through normal snapshot round trips;
- sediment mass/thickness inversion and burial-compaction regression for equal added mass at shallow versus deep burial;
- fluvial incision shuts off at zero runoff, while a zero-runoff integration still performs conservative slope/regolith-driven hillslope sediment transport;
- regolith production declines exponentially with existing mantle thickness, shuts off for zero continental fraction, and gives timestep-invariant analytical weathering for a stable plate interior;
- seed-42 convergent-uplift footprint width/contour complexity regression, 64-seed tectonic robustness/diversity sweep, and 256-seed anti-clustering guard;
- Godot debug-map response arrays and presentation checks for terrain-driving uplift/divergence colors and explicit plate topology outlines.

`worldsim_sanitizer_smoke` is a bounded scenario intended for ASan/UBSan builds.

## Geology plausibility benchmark

`worldsim_geology_benchmark` is a non-authoritative diagnostic executable that measures the tectonic generator and initialized stateful geology surface across multiple seeds. It records plate-area/spacing diversity, resolved boundary kinematic mix, crust connectivity, hypsometry, geological boundary features, and tectonic/topographic coupling using sphere-native sampling.

Reference mismatches are emitted as warnings rather than CTest failures because the current generator has not yet been scientifically calibrated. CI runs the 64-seed baseline and publishes `worldsim-geology-benchmark` for review. Methodology, reference sources, warning semantics, and explicitly unsupported scientific claims are documented in `docs/GEOLOGY_VALIDATION.md`.


## Pre-fix geology plausibility baseline — 2026-09-08

Corrected pull-request CI run `34182628679` evaluated 64 consecutive seeds with 4,096 equal-area probes per seed and a level-5 uniform cube-sphere topology cover. Both Core/GCC and Godot 4.7/Linux jobs completed successfully.

Measured aggregate baseline:

- plate-area coefficient of variation: 0.0577 mean (0.0369..0.0886);
- largest/smallest plate-area ratio: 1.2385 mean (1.1405..1.3810);
- plate-center nearest-neighbor spacing CV: 0.0616 mean;
- coarse boundary-length mix using the PB2002 +/-20 degree strike-slip rule: 39.45% convergent, 39.19% divergent, 21.36% transform;
- above-sea land fraction: 25.50% mean (17.54%..31.75%);
- coarse ocean and land elevation modes: -4,375 m and +625 m;
- high-affinity crust connected components: 7.05 mean (3..14);
- crust-matched uplift macro-relief excess: +540 m mean.

The benchmark emitted exactly two reference warnings:

1. `plate_geometry_too_regular`: plate areas and seed-center spacing are too uniform to express a PB2002-like hierarchy of plate scales.
2. `earthlike_land_mode_displaced`: the generated land-elevation mode is more than 500 m above the modern-Earth near-sea-level reference.

The corrected boundary-kinematics mix does **not** trigger the broad PB2002 comparison warning. The earlier exploratory 45-degree transform split was rejected before merge because Bird's published classification uses +/-20 degrees from boundary azimuth.

These results establish a pre-fix baseline only. They do not imply that unmeasured processes such as plate-age evolution, physical Euler rates, subduction, crustal thickness, isostasy or erosion are scientifically validated.

### Corrected plate-layout baseline

After plate diversification and the minimum-separation correction, PR #17 CI run `34184463614` repeated the same 64-seed / 4,096-probe / level-5 benchmark. Core/GCC, CTest and the benchmark completed successfully.

Compared with the original pre-fix baseline:

- plate-area CV: 0.0577 -> 0.1924 mean;
- largest/smallest plate-area ratio: 1.2385 -> 2.1310 mean;
- plate-center nearest-neighbor spacing CV: 0.0616 -> 0.2358 mean;
- coarse boundary mix: 39.06% convergent, 38.32% divergent, 22.62% transform;
- above-sea land fraction: 25.50% mean (17.47%..31.66%);
- crust-matched uplift macro-relief excess: +551 m mean.

The benchmark reports `INFO no_broad_plausibility_warnings`. The anti-clustering guard changes only conflicting seed placements; relative to the unconstrained diversified layout from #16, the mean largest/smallest area ratio changes from 2.1998 to 2.1310 and the boundary fractions move by less than 0.1 percentage point.

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

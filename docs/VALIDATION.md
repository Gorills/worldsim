# Validation record

This file distinguishes what was actually executed from architectural intent.

## Current audit

The 2026-09-08 correctness audit, reproduced failures, design references and actual validation results are recorded in [AUDIT_2026-09-08.md](AUDIT_2026-09-08.md). Its dedicated CTest target is `worldsim_audit_tests`. Climate v2 was implemented after that audit and is recorded in [CLIMATE.md](CLIMATE.md). Earlier dated records below are historical and do not describe current snapshot compatibility or terrain-viewer authority.

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
- rejection of stale authoritative-world snapshot epochs (versions 2 through 17);
- adaptive LOD stability;
- snapshot restore from a different current LOD cover;
- command routing when the addressed coarse cell has been refined;
- columnar field-store consistency;
- cohort spatial index consistency and count-conserving indexed redistribution;
- non-negative/finiteness ecology invariants;
- regolith-aware soil-water storage: thin substrate produces less root-zone storage and more runoff than deep regolith under the same forcing;
- living-soil coupling: fertility limits NPP, vegetation turnover creates litter, and litter-rich soil feeds back toward higher reduced fertility;
- adaptive-cover adjacency: coarse/fine neighbor regions resolve to deterministic active leaves with area-normalized weights that close to one;
- flora v1: PFT carbon sums to aggregate vegetation, sterile worlds remain sterile without propagules, neighbor recruitment colonizes suitable empty habitat, and woody canopy suppresses grass under otherwise matched conditions;
- fauna v2: herbivores select neighboring forage, carnivores select neighboring prey, and frozen movement plans prevent same-tick multi-hop migration;
- C ABI field discovery and value copying;
- module-extension contract and generic snapshot support;
- post-load spatial-store/active-cover validation through normal snapshot round trips;
- sediment mass/thickness inversion and burial-compaction regression for equal added mass at shallow versus deep burial;
- fluvial incision shuts off at zero runoff, while a zero-runoff integration still performs conservative slope/regolith-driven hillslope sediment transport;
- hillslope transport remains creep-like at low gradients, accelerates strongly near a critical slope, and stays finite/bounded for supercritical gradients;
- terrestrial drainage terminates at the first submerged receiver, while zero-runoff/zero-regolith marine sediment still redistributes conservatively downslope with weaker transport in deep water;
- regolith production declines exponentially with existing mantle thickness, shuts off for zero continental fraction, and gives timestep-invariant analytical weathering for a stable plate interior;
- seed-42 convergent-uplift footprint width/contour complexity regression, 64-seed tectonic robustness/diversity sweep, and 256-seed anti-clustering guard;
- Godot debug-map response arrays and presentation checks for terrain-driving uplift/divergence colors and explicit plate topology outlines.

The Godot headless integration scripts additionally verify that reconstructed
walking heights respond to authoritative elevation changes, adjacent terrain
patches share identical boundary samples, adaptive-cover changes advance the
terrain revision, and refreshed `ArrayMesh` and `HeightMapShape3D` resources use
the same height array.

`ci_simulation_lab.gd` instantiates the full-world laboratory in Russian, checks
its 512 x 256 live map, confirms hydrology and ecology fields are selectable,
switches the scalar layer, inspects a cell, advances one day and verifies that
selected-field statistics and history update. The lower-level adapter smoke also
checks generic elevation agreement, extensive-value/area density normalization,
field inspection and focused LOD-map refinement.

`worldsim_sanitizer_smoke` is a bounded scenario intended for ASan/UBSan builds.

`worldsim_climate_tests` covers the coupled climate process contract: optional
magic composition, cumulative energy accounting, closed atmospheric/ocean/land
water inventory, land/ocean seasonal amplitude and phase, orographic rainfall,
focus-LOD invariance, and snapshot continuation. The
`worldsim_climate_dump_smoke` CTest target exercises the inspectable ledger/map
export path.

The 2026-09-08 Climate v2 integration run completed all 10 CTest targets. Its
730-day adaptive diagnostic closed planet water to `2.06951e-10` relative and
surface energy to `8.98413e-13` relative. The exact process, sanitizer and Godot
evidence is recorded in `CLIMATE.md`; climatology ranges remain diagnostics,
not calibration claims.

## Geology plausibility benchmark

`worldsim_geology_benchmark` is a non-authoritative diagnostic executable that measures the tectonic generator and initialized stateful geology surface across multiple seeds. It records plate-area/spacing diversity, resolved boundary kinematic mix, crust connectivity, hypsometry, geological boundary features, and tectonic/topographic coupling using sphere-native sampling.

Reference mismatches are emitted as warnings rather than CTest failures because the current generator has not yet been scientifically calibrated. CI runs the 64-seed baseline and publishes `worldsim-geology-benchmark` for review. Methodology, reference sources, warning semantics, and explicitly unsupported scientific claims are documented in `docs/GEOLOGY_VALIDATION.md`.

## Ecology validation scope

The default ecology now has a living-soil substrate, propagule-limited grass/shrub/tree functional types, and reduced habitat-selected fauna redistribution across the shared adaptive-cover adjacency contract. Executable contracts and explicit limitations are documented in `docs/ECOLOGY_VALIDATION.md`. The fertility field remains an index rather than a claimed nitrogen/phosphorus mass budget, and fauna movement is population redistribution rather than an individual trajectory model.


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

## Basin hydrology (2026-09-08)

The new domain implementation and its modeling limits are documented in
[HYDROLOGY.md](HYDROLOGY.md). Validation includes seven hydrology process groups,
full-core CTest, sanitizer runs, Godot field introspection and a 730-day
full-world adaptive-cover diagnostic. The latter closes the land-water budget
with maximum relative residual 2.70362e-14. Reference-grid river geometry is
independent of camera refinement; this does not establish spatial convergence
of an adaptive hydraulic solver. Basin hydrology introduced epoch 17; snapshot
epoch 18 with persistent climate state is current.

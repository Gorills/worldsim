# Validation policy

This document defines current validation expectations. It is not a chronological test journal.

Historical run details belong in GitHub pull requests, commits and Actions artifacts. Do not append dated audit sections here after each change.

## What CI currently proves

The normal Linux CI exercises three different concerns:

- **Core correctness:** configure/build plus the repository CTest suites.
- **Natural-world regression smoke:** a short coupled multi-seed L2/L3 matrix using the existing `worldsim_long_run --assert-stable` envelope.
- **Engine integration:** Godot 4.7 GDExtension build, headless runtime tests, the walking/viewer scene, simulation laboratory and rendered visual capture.

The geology benchmark and visual diagnostics are broad diagnostic evidence. They are not scientific calibration gates unless a specific executable regression says otherwise.

## Frozen natural-world baseline

The current natural-world feature baseline is epoch 39 on `main` commit `1f0ac71b8488a58fd18dcb713ffc07e51374062d`.

Validation PR #57 / CI #492 additionally ran 100 coupled years for seeds `0,42,999` at uniform levels `2,3`. All six cases passed `--assert-stable`; the worlds remained productive and bounded and the planetary conservation residuals remained at floating-point noise scale.

The exact long-run command, interpretation and freeze/reopen rule are documented in `LONG_RUN_STABILITY.md`.

A green natural-world baseline does **not** claim:

- Earth calibration;
- statistical identity of all L2/L3 observables;
- production performance on target player hardware;
- correctness of gameplay systems that do not exist yet.

## Meaning of a failed diagnostic

A diagnostic difference is not automatically a product defect.

Before changing production simulation code, reduce the observation to a bounded reproduction of one of these:

- conservation failure;
- invalid/non-finite state;
- determinism or snapshot-continuation failure;
- timestep-dependent semantics that violate the declared contract;
- LOD/refine/coarsen behavior that violates the declared domain semantics;
- reproducible pathological world behavior;
- a player-facing action that cannot be implemented correctly with the current authoritative state.

If none can be demonstrated, record the diagnostic if useful and continue feature work.

## Validation for new player-facing slices

Every survival-substrate slice must add tests at the authority boundary it introduces.

For `Resource Acquisition v1`, validation must cover at least:

1. valid collection transfers the realized amount exactly once;
2. unavailable/invalid requests leave authoritative state unchanged;
3. source stocks cannot become negative;
4. persistent player/resource state survives snapshot round trips;
5. declared LOD semantics preserve resource quantity or availability correctly;
6. same seed and same commands continue deterministically under the existing contract;
7. the Godot client exercises the same authoritative path rather than a presentation-only substitute;
8. existing natural-world/core tests and the normal CI stability smoke stay green.

Later crafting, building and survival-needs slices should follow the same rule: test the player-visible state transition, persistence and domain accounting that the feature actually owns instead of adding broad speculative validation infrastructure.

## Sanitizers and platform checks

Sanitizer or extra-platform runs are risk-driven, not mandatory ceremony for every documentation or UI-only change. Use them when a change touches unsafe memory boundaries, serialization, numerical conversion, ownership/lifetime, platform-specific integration or other code where the additional evidence addresses a concrete risk.

Do not claim a sanitizer, platform, performance target or hardware profile was verified unless that exact check was executed.
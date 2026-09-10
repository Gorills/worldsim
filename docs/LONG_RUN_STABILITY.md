# Natural-world long-run stability contract

This document defines the current long-run regression contract for the frozen natural-world vertical slice. It is not a roadmap and it is not an Earth-calibration record.

## Reproduce the baseline

The default repository entry point is:

```bash
make stability
```

It runs the existing `worldsim_long_run` executable for:

- 100 coupled years;
- seeds `0,42,999`;
- uniform levels `2,3`;
- `--assert-stable` enabled.

The orchestration script writes per-case histories, a final matrix, adjacent-resolution diagnostics and `summary.json` under `out/stability-matrix`.

The C++ harness owns the per-case stability envelope. The Python driver does not introduce a cross-resolution acceptance threshold.

## Current frozen baseline

The current feature baseline is epoch 39 on `main` commit `1f0ac71b8488a58fd18dcb713ffc07e51374062d`.

Validation PR #57 used CI run #492 to execute the six 100-year cases independently so the same existing harness could complete in bounded wall-clock time. No natural-world production code was changed for that validation.

All six cases passed `--assert-stable`.

At year 100 across the six worlds:

- vegetation stock remained approximately `0.805x..1.128x` its initialized stock;
- vegetation density remained approximately `0.371..0.679 kgC/m²`;
- global NPP remained approximately `10.49..14.61 PgC/year`;
- vegetated land fraction remained approximately `0.931..1.000`;
- the worst connected-component vegetation ratio remained approximately `0.632..1.022`;
- fauna carbon ratio remained approximately `0.691..1.985`;
- registered invalid values were zero;
- planet carbon, nitrogen and water residuals remained around floating-point `1e-14` scale.

Across the full trajectories, the minimum global vegetation ratio was approximately `0.767`, maximum collapsed-baseline area was approximately 5%, and NPP remained positive in every case.

This is sufficient evidence to freeze the current natural-world vertical slice for feature development.

## What `--assert-stable` protects

The long-run harness is an engineering failure detector. It rejects invalid/non-finite registered state and protects broad boundedness/conservation conditions including planetary carbon/nitrogen closure, vegetation persistence and fauna biomass bounds.

Its thresholds are deliberately broad. Passing them means the reduced model did not exhibit one of the declared catastrophic engineering failures over the run. It does not mean every observable has converged to a scientifically correct value.

## Cross-resolution diagnostics

L2/L3 differences are recorded for inspection but are **diagnostic only** unless a separate bounded test establishes an invariant that the difference violates.

The 100-year epoch-39 baseline still shows substantial path-dependent differences. Examples at year 100 include:

- seed 42 annual burned land fraction: about 1.77% at L2 versus 6.44% at L3;
- seed 999 annual burned land fraction: 0% at L2 versus about 2.84% at L3;
- mineral-nitrogen density differences of roughly 33–48% depending on seed;
- fauna-carbon-ratio differences of roughly 29–40% depending on seed.

These observations are not hidden. They also did not localize a conservation, determinism, timestep or LOD-semantic failure, and all six worlds remained bounded and productive.

Do not create a backlog item merely because a nonlinear/path-dependent coupled observable differs after 100 years. First reproduce a specific broken contract under controlled conditions.

## Freeze/reopen rule

Natural-world calibration and generic convergence work is frozen.

Reopen a natural-world subsystem only if a bounded reproduction demonstrates at least one of:

1. conservation failure;
2. invalid/non-finite or otherwise pathological state;
3. deterministic continuation/snapshot failure;
4. timestep or LOD behavior that violates a declared semantic contract;
5. gameplay-blocking world behavior encountered by the solo-survival work;
6. a narrowly required authoritative world capability that the survival milestone cannot implement without.

When a reproduction exists, fix the smallest verified failure mode and add a regression that fails before the fix.

## When to run the full matrix again

The normal PR CI uses a short smoke because the full 100-year matrix is expensive.

Run the full matrix again when a change materially affects long-run natural-world dynamics, conservation or resource renewal/depletion semantics. It is not required for documentation-only work or for player-facing changes that do not alter those dynamics.

If the survival substrate later introduces extraction fluxes into ecological, hydrological or geological stocks, add an appropriate long-run/player-use regression for those new boundaries rather than reopening unrelated natural-world calibration.
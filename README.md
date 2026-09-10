# WorldSim

WorldSim is a C++20 persistent planet-scale world kernel with a Godot 4.7 client. The simulation owns authoritative world state; rendering and UI consume that state through engine boundaries.

The current natural world includes stateful geography/geology, coupled climate and hydrology, soil carbon and nitrogen, grass/shrub/tree vegetation, wildfire and cohort fauna. These systems form a living-world substrate, not a scientifically calibrated Earth model.

## Current development direction

The current product milestone is **one player surviving in the living world without NPC support**.

Before population, settlements, trade, institutions or other society-scale systems are started, the player must be able to use the world itself to obtain water and food, gather wood and minerals, make tools, build shelter and satisfy the implemented survival needs. Resource availability, depletion and recovery must belong to authoritative persistent world state rather than presentation-only spawned items.

The natural-world simulation is frozen for feature development after the epoch-39 100-year validation baseline. It is reopened only for a reproduced invariant violation, pathological world state, gameplay-blocking behavior or a narrowly required capability for the survival milestone.

**Next major slice: Resource Acquisition v1** — connect the walking player to authoritative water, food, biomass and geological resources plus persistent inventory. See [docs/DIRECTION.md](docs/DIRECTION.md) for the completion gate, development order and bounded scope.

## Design invariants

1. **The simulation owns the world; the renderer does not.** Godot, a server or tooling consumes authoritative state through explicit boundaries.
2. **Simulation LOD is state aggregation/refinement, not object sleeping.** Far-away state remains authoritative at coarser resolution.
3. **LOD transitions preserve domain semantics and conserved quantities.** Extensive fields split/sum; intensive fields use declared aggregation rules.
4. **Domains are modules.** Modules register fields, state stores and systems instead of adding domain special cases to the kernel.
5. **Systems declare data access and ordering.** The scheduler rejects unordered read/write conflicts.
6. **Time is fixed-step and deterministic within the existing build/toolchain contract for equal seed and inputs.** Randomness is stateless and keyed by stable identifiers.
7. **Snapshots are continuation state.** Authoritative spatial cover, fields, stores, commands/events and gameplay state must survive save/load.
8. **Rendering LOD and simulation LOD remain separate.** Presentation detail cannot silently become authoritative world state.

## Repository layout

```text
include/worldsim/      Public C++ and C ABI
src/                   Simulation kernel and default domain modules
tests/                 Core/domain/LOD/snapshot regression tests
apps/                   Headless diagnostics and CLI tools
godot/                  Godot 4.7 GDExtension adapter and client
docs/                   Current architecture/domain/product contracts
scripts/                Reproducible developer entry points
```

## Build and test core

Requirements: CMake 3.24+, Ninja and a C++20 compiler.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Equivalent entry points:

```bash
make core
make test
```

## Run the Godot client

The adapter targets Godot 4.7 and currently pins `godot-cpp` `10.0.0-rc2`. `godot-cpp` is fetched during configure unless supplied under `third_party/godot-cpp`.

```bash
cmake --preset godot-dev
cmake --build --preset godot-dev --target worldsim_godot
make run
```

`make run` launches the walking world client. `make map` opens the macro world map, and `make lab` opens the full-world simulation laboratory.

## Diagnostics

The repository keeps diagnostics as reproducible tools, not as development direction.

```bash
make stability   # 100-year seeds 0,42,999 at L2/L3 with --assert-stable
make climate     # two-year climate diagnostic
make geology     # broad geology plausibility benchmark
make visual      # C++ visual diagnostic suite
make lab         # interactive full-world field laboratory
```

`make stability` is the natural-world regression matrix. Its current baseline and the rules for interpreting cross-resolution diagnostics are in [docs/LONG_RUN_STABILITY.md](docs/LONG_RUN_STABILITY.md). General CI/validation policy is in [docs/VALIDATION.md](docs/VALIDATION.md).

## Engine boundary

`worldsim_c` is a shared C ABI over the same simulation kernel used by the native C++ tools. The existing boundary exposes active cells, field discovery/value arrays, focus/LOD control, scheduled field impulses, pending simulation events and snapshot save/load. The Godot GDExtension is a client of this boundary and related native adapter APIs; UI state must not become a second source of truth.

Player-facing resource acquisition will add the smallest authoritative query/command surface required by `Resource Acquisition v1`, rather than introducing a parallel Godot-only inventory or resource model.

## Documentation

The main current contracts are:

- [docs/DIRECTION.md](docs/DIRECTION.md) — authoritative product direction and next slice;
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — kernel, scheduling, state and LOD architecture;
- [docs/ADDING_MODULES.md](docs/ADDING_MODULES.md) — domain extension model;
- [docs/ENGINE_ABI.md](docs/ENGINE_ABI.md) — engine boundary;
- [docs/TERRAIN_SLICE.md](docs/TERRAIN_SLICE.md) and [docs/LIVING_SURFACE.md](docs/LIVING_SURFACE.md) — current terrain/living-surface presentation contracts;
- [docs/CLIMATE.md](docs/CLIMATE.md), [docs/HYDROLOGY.md](docs/HYDROLOGY.md), [docs/CARBON_CYCLE.md](docs/CARBON_CYCLE.md), [docs/NITROGEN_CYCLE.md](docs/NITROGEN_CYCLE.md), [docs/SOIL_CARBON.md](docs/SOIL_CARBON.md), [docs/FIRE.md](docs/FIRE.md) and [docs/ECOLOGY_VALIDATION.md](docs/ECOLOGY_VALIDATION.md) — natural-world domain contracts;
- [docs/GEOLOGY_VALIDATION.md](docs/GEOLOGY_VALIDATION.md) — geology benchmark methodology and model scope;
- [docs/GODOT_4_7.md](docs/GODOT_4_7.md) — Godot integration contract;
- [docs/SIMULATION_LAB.md](docs/SIMULATION_LAB.md) and [docs/VISUAL_DIAGNOSTICS.md](docs/VISUAL_DIAGNOSTICS.md) — diagnostic tooling.

Historical audits and per-PR validation journals do not belong in the current documentation tree. Git history, pull requests and Actions artifacts are the historical record.

## Not current work

The following are intentionally outside the current milestone: NPC population simulation, settlements, production specialization, markets, institutions, diplomacy, warfare, epidemiology, broad scientific Earth calibration and generic natural-world convergence tuning.

Those areas are reconsidered only after the solo-survival completion gate in `docs/DIRECTION.md` is satisfied.

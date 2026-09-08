# Architecture

## 1. Authority boundary

`worldsim` is a headless authoritative simulation library. No Godot type appears in the kernel. Engine adapters translate kernel state into render/gameplay data.

```text
                 ┌──────────────────────────┐
                 │      Simulation core     │
                 │ spatial/state/scheduler  │
                 └─────────────┬────────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
          C ABI shared     GDExtension      headless/server
             library          adapter           tooling
              │                │
       any native engine      Godot 4.7
```

The rule is one-way: an adapter can depend on the kernel; the kernel cannot depend on an adapter.

## 2. Spatial model

The planet uses a cube-sphere. Each of six cube faces is a quadtree. `CellId` encodes face, level, x, and y in a stable 64-bit value.

A world cover is a set of non-overlapping active leaf cells. Leaves may be at different levels. Refinement replaces one leaf with four children; coarsening replaces four sibling leaves with their parent.

This gives:

- a bounded-neighbor spherical topology;
- stable parent/child addressing;
- exact hierarchical aggregation boundaries;
- no poles or singular longitude cell at the data-model level;
- a direct mechanism for simulation LOD.

The current focus policy is intentionally a policy layer inside `Simulation::target_level()`. The hierarchy/state contracts do not depend on the particular angular thresholds. Refinement uses the nominal angular thresholds while coarsening requires a 4-degree exit margin, preventing focus jitter around a boundary from repeatedly transforming authoritative state. A production project can replace the policy with camera interest, settlement importance, active wars, player subscriptions, server budget, or a combined importance function.

`CubeSphereTopology::neighbors4()` remains a same-level topological query. Adaptive systems use `WorldState::resolve_active_cover(region)`, which resolves an arbitrary hierarchy region to the active cell or active descendants that currently represent it and returns deterministic area-normalized weights summing to one. `WorldState::active_neighbors4(cell)` applies that contract to the four same-level neighbor regions of an active source cell. This allows geology transport and ecology dispersal to share one coarse/fine interface rule instead of treating `neighbors4()` as if it returned active leaves.

The current world cover does not require a 2:1 balance constraint: the resolver supports an active coarse ancestor or multiple active descendants across the requested region. This is a deliberate non-conforming AMR-style contract. Established AMR libraries such as p4est explicitly support coarse/fine non-conforming interfaces and commonly add 2:1 balancing as a separate mesh-policy operation rather than conflating it with neighbor identity: https://p4est.github.io/api/p4est-2.8.6/p4est__extended_8h.html. If future transport requires face-length fluxes rather than region-average transfer weights, add that geometry explicitly instead of reinterpreting these area weights.

## 3. State semantics

### Spatial fields

`FieldRegistry` defines each field once with:

- stable string key;
- unit;
- `Intensive` or `Extensive` semantics;
- default/min/max values.

`FieldStore` is columnar (SoA): one contiguous vector per field plus a dense cell table. Systems can iterate columns without a map lookup per value.

**Extensive** quantities scale with area/population and are conserved across LOD by split/sum. Examples: water volume, vegetation carbon, mana energy, ore stock, stored food, treasury money when spatially attributed.

**Intensive** quantities describe local state independent of represented area. They copy on refinement and use area-weighted aggregation on coarsening. Examples: temperature, humidity ratio, fertility index.

Not every future variable is correctly modeled by these two semantics. A new state store is appropriate when aggregation needs domain rules: price distributions, legal ownership, epidemic compartments, armies, trade networks, political actors, etc. The core provides `IStateStore::on_refine/on_coarsen` specifically for this reason.

### Structured stores

`CohortStore` demonstrates non-field state. Fauna is represented by cohorts rather than one object per animal. Its LOD hooks split counts on refinement and merge compatible lineages on coarsening while preserving total count.

Future structured stores should own their aggregation semantics rather than encoding them as arbitrary fields.

## 4. Modules and systems

`ISimModule` has four bounded responsibilities:

1. register fields;
2. register structured stores;
3. register systems;
4. initialize its state.

`ISimSystem` declares:

- stable system id;
- cadence in ticks;
- explicit `after()` dependencies;
- read/write resource keys;
- `step()`.

During `Scheduler::finalize()`, the dependency graph is topologically sorted. If two systems conflict on declared resources and no dependency orders them, build fails. This prevents accidental simulation behavior from depending on module registration order.

The current scheduler is serial. Its declarations deliberately form a future-safe boundary for parallel batches: parallel execution can later be introduced only for systems whose declared accesses prove independence, without changing domain code.

## 5. Time and determinism

The authoritative clock uses a fixed timestep (`SimulationConfig::tick_seconds`). System cadence is an integer number of ticks. A cadence-`N` system executes at the **end** of each completed `N`-tick window and receives `N * tick_seconds` as its integration interval; it does not advance `N` ticks at world tick zero. This keeps every domain aligned to authoritative elapsed time.

Randomness uses a stateless hash of:

```text
world seed + random stream id + tick + object id
```

This avoids mutable global PRNG order dependence when systems or iteration structures change.

The test contract currently guarantees reproducibility for same seed + same input in the tested build. Bit-exact cross-compiler/cross-architecture determinism is not claimed; floating-point math and libm remain a portability boundary.

## 6. LOD transition contract

Refinement/coarsening is an authoritative state transition, not a visualization optimization.

Every `IStateStore` receives the transition. A transition is valid only if all stores can transform their representation while preserving their domain invariants.

Tests currently assert:

- global cube-sphere area closure;
- extensive-field conservation;
- intensive-field preservation/aggregation;
- cohort count conservation;
- stable focus without refine/coarsen oscillation;
- commands addressed to a coarse region resolve to the active leaf that currently covers it;
- snapshots restore an adaptive cover even when the current cover is different.

## 7. Commands and events

External actions enter the world as scheduled commands rather than direct renderer-owned mutation. The included `FieldImpulseCommand` is the minimal command type and demonstrates deterministic `(tick, sequence)` ordering.

Simulation events are output facts emitted by systems. They are persisted in snapshots until drained, so an engine can consume them without making event delivery part of authoritative state mutation.

Production domains should introduce typed command payloads and typed event schemas rather than overload field impulses for everything.

## 8. Snapshots

Snapshot version 14 contains:

- magic header and format version;
- field schema hash;
- simulation config and seed;
- tick;
- LOD focus;
- command sequence and queued commands;
- exact adaptive active-cell cover;
- pending events;
- a versioned chunk for every registered state store.

Primitive values use explicit little-endian encoding and IEEE-754 floats. Native C++ struct layout is never serialized.

A snapshot is rejected if the field schema, simulation config, seed, store set, store version, cell cover, or framing is incompatible. After store chunks are loaded, spatial stores validate that their state references the reconstructed active cover; the core field store additionally rejects invalid/non-finite/out-of-bounds values.

Snapshot v14 is the current compatibility epoch for authoritative world state. Version 2 may contain the old fixed-continent terrain, version 3 the pre-orogenic tectonic terrain, version 4 the pre-diversification plate layout, version 5 the unconstrained diversified layout, version 6 the minimum-separation static-geography model before persistent geology state, version 7 the first stateful-geology model before sediment mass used burial-dependent compaction, version 8 the compacted-sediment model before fluvial incision was separated from water-independent hillslope creep, version 9 the separated geomorphology model before regolith production became depth-dependent, version 10 the depth-dependent-regolith model before hillslope transport gained critical-slope acceleration, version 11 the critical-slope model before terrestrial drainage terminated at sea level and submerged sediment gained a dedicated marine-routing closure, version 12 the coast-to-basin geology model before living-soil ecology, and version 13 the living-soil model before persistent grass/shrub/tree functional-type pools and propagule-limited vegetation dynamics. Versions 2 through 13 are rejected because silently accepting them could mix incompatible authoritative world semantics. Long-term save compatibility should be implemented as explicit snapshot migrations. Do not silently deserialize old bytes into a changed model.

## 9. Why magic does not contaminate the core

`MagicModule` is intentionally an ordinary module. It registers mana and magic-effect fields and systems. Other modules may explicitly read those resources.

The dependency direction is:

```text
core contracts <- domain module <- cross-domain system dependencies
```

not:

```text
core if (magic) ...
```

A world without magic can omit the module. A world with ley lines, divine effects, corruption, elemental climates, or magical species can add stores/systems whose resources are scheduled like every other domain.

## 10. Scaling path

The current code intentionally establishes contracts needed before optimization. Production scaling should proceed without changing those contracts:

1. benchmark actual active-cell/population workloads;
2. batch systems over contiguous columns;
3. build explicit neighbor/transport caches for a fixed cover epoch;
4. parallelize conflict-free scheduler batches;
5. partition spatial covers across simulation workers/processes if one process no longer fits the budget;
6. persist cold regions/chunks outside RAM if required;
7. run high-fidelity local solvers only where the domain actually requires them.

The invalid scaling path is to instantiate every conceptual animal/person/plant as an always-resident engine object and attempt to hide the cost with rendering LOD.

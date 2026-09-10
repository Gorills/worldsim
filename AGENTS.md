# Engineering rules

These rules are mandatory for every implementation change in this repository.

## Product direction

Read `docs/DIRECTION.md` before starting feature work. It is the authoritative development direction.

The current natural-world vertical slice is frozen for feature development after the epoch-39 100-year validation baseline. Do not turn calibration questions or generic long-horizon L2/L3 diagnostic deltas into implementation work unless a bounded reproduction demonstrates a conservation, determinism, snapshot, timestep, LOD or gameplay-blocking failure.

The current milestone is **solo survival in a living world**. Before population, settlements, trade or other society-scale systems are introduced, one player must be able to acquire world resources, make tools, build shelter and satisfy the implemented survival needs using authoritative persistent world state.

The next major slice is `Resource Acquisition v1` as defined in `docs/DIRECTION.md`. Do not pull crafting, buildings, needs, NPCs or economy into that slice.

## Documentation-first rule

Before changing behavior, architecture, performance-sensitive code, engine integration, serialization, simulation LOD, rendering, input, UI, or localization:

1. Identify the exact current behavior and the contracts/callers/tests affected.
2. Read the authoritative documentation for the exact versions used by this repository.
3. For architectural or performance work, compare the proposed approach with at least one established implementation or published practice from a comparable simulation/game/engine system.
4. Record non-trivial references and decisions in the pull request or an enduring technical contract. Do not add dated audit journals or per-PR validation history to `docs/`; Git history, pull requests and Actions artifacts are the historical record.
5. Do not invent engine APIs, serialization guarantees, performance claims, platform behavior or model properties. If a fact cannot be verified, mark it NOT VERIFIED and do not build a critical design assumption on it.
6. Prefer the smallest change that fixes the verified failure mode or completes the bounded product slice. Do not add speculative abstractions or future infrastructure.
7. Add or update tests so the original failure mode or slice contract would be detected, then run the checks appropriate to the risk before merging.

For Godot work, use the official documentation matching the target Godot version and the matching godot-cpp API/release. For adaptive world-simulation work, verify LOD/state-transition decisions against established hierarchical spatial/AMR or large-world simulation practice in addition to this repository's existing contracts.

## Architectural invariants

- The simulation kernel is authoritative and has no Godot dependency.
- Rendering LOD and simulation LOD are separate concerns.
- Simulation LOD transitions must preserve domain invariants and conserved quantities.
- New cross-cell transport code must not assume that `CubeSphereTopology::neighbors4()` returns active-cover neighbors when the cover is adaptive; use the active-cover adjacency contract.
- Engine/UI code may consume simulation state and submit commands but must not become the authoritative owner of gameplay or world state.
- Resource extraction must not create duplicate material: the authoritative source/depletion contract and the receiving persistent inventory state must agree on the realized transfer.
- New player-facing state must define snapshot and LOD semantics before it is exposed through Godot.

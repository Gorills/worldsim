# Engineering rules

These rules are mandatory for every implementation change in this repository.

## Documentation-first rule

Before changing behavior, architecture, performance-sensitive code, engine integration, serialization, simulation LOD, rendering, input, UI, or localization:

1. Identify the exact current behavior and the contracts/callers/tests affected.
2. Read the authoritative documentation for the exact versions used by this repository.
3. For architectural or performance work, compare the proposed approach with at least one established implementation or published practice from a comparable simulation/game/engine system.
4. Record the relevant references in the pull request, commit notes, or an audit/design document when the decision is non-trivial.
5. Do not invent engine APIs, serialization guarantees, performance claims, or platform behavior. If a fact cannot be verified, mark it NOT VERIFIED and do not build a critical design assumption on it.
6. Prefer the smallest change that fixes the verified failure mode. Do not add speculative abstractions or future infrastructure.
7. Add or update tests so the original failure mode would be detected, then run the checks appropriate to the risk before merging.

For Godot work, use the official documentation matching the target Godot version and the matching godot-cpp API/release. For adaptive world-simulation work, verify LOD/state-transition decisions against established hierarchical spatial/AMR or large-world simulation practice in addition to this repository's existing contracts.

## Architectural invariants

- The simulation kernel is authoritative and has no Godot dependency.
- Rendering LOD and simulation LOD are separate concerns.
- Simulation LOD transitions must preserve domain invariants and conserved quantities.
- New cross-cell transport code must not assume that CubeSphereTopology::neighbors4() returns active-cover neighbors when the cover is adaptive; mixed-level active-neighbor resolution is a separate required contract.
- Engine/UI code may consume simulation state but must not become the authoritative owner of it.

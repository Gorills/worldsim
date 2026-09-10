# Product direction: solo survival in a living world

This document is the authoritative product direction for WorldSim. Technical domain documents describe how existing systems work; they do not define development priority.

## Goal

Before WorldSim adds simulated people, settlements, trade, institutions or other society-scale systems, one player must be able to live directly from the authoritative natural world.

The milestone is not a survival-themed UI over spawned items. The world itself must provide the usable water, food, biomass, stone and mineral resources. Player actions must consume or transform authoritative state, and the consequences must persist through time, LOD transitions and snapshot save/load.

The target experience is:

```text
living world
  -> inspect local conditions and resources
  -> acquire water / food / raw materials
  -> carry and store materials
  -> make tools
  -> build shelter and basic infrastructure
  -> use renewable and finite resources over time
  -> remain alive without NPC support
```

Only after that chain works end to end do population, settlements, trade and other human systems become current work.

## Natural-world baseline is frozen

The current natural-world vertical slice is the epoch-39 baseline on `main` at commit `1f0ac71b8488a58fd18dcb713ffc07e51374062d`.

Validation PR #57 / CI #492 ran the existing coupled model for 100 years across seeds `0,42,999` at uniform levels `2,3`. All six cases passed the existing `--assert-stable` envelope. The worlds remained bounded, productive and conservative; the result is recorded in `LONG_RUN_STABILITY.md`.

Natural-world work is therefore frozen for feature development. Do not reopen climate, hydrology, geology, ecology, fire, fauna or nutrient calibration merely because a long-horizon L2/L3 observable differs.

Reopen a natural-world subsystem only when at least one of these is demonstrated with a bounded reproduction:

1. a conservation, determinism, snapshot, timestep or LOD invariant is violated;
2. a reproducible pathological world state makes the intended gameplay impossible;
3. a player action exposes missing or incorrect authoritative state semantics;
4. the solo-survival milestone requires a narrowly scoped world capability that does not exist yet.

Scientific Earth calibration and generic convergence improvement are not current product goals.

## Solo-survival completion gate

The solo-survival milestone is complete when one player can, using only world-provided resources:

- locate and collect fresh water, then consume it;
- obtain food from at least one renewable natural source;
- gather structural biomass such as wood/fiber;
- gather stone and extract at least one finite mineral resource;
- carry and store acquired materials in authoritative persistent state;
- craft at least one useful tool from gathered inputs;
- use that tool to improve access to or efficiency of a resource;
- construct a basic shelter from gathered materials;
- create/use a basic heat or fire source where the survival model requires it;
- satisfy implemented thirst, hunger and exposure constraints without NPC assistance;
- save, reload and continue with player state, structures, depleted resources and natural-world state intact.

A scripted end-to-end survival scenario must exercise the same commands/state used by the playable Godot client. It must not rely on test-only item injection.

## Development order

1. **Resource Acquisition v1 — NEXT.** Connect the walking player to authoritative natural resources and persistent inventory.
2. **Crafting and Tool Use v1.** Turn gathered materials into a small set of functional tools; tools affect real acquisition actions.
3. **Shelter and Fire v1.** Place persistent structures/heat sources that consume gathered materials and interact with local conditions.
4. **Survival Needs v1.** Thirst, hunger and exposure consume the resources/capabilities above; add the smallest renewable food loop needed to sustain one player.
5. **Solo-survival gate.** Run the complete player-visible loop, close remaining gameplay-blocking gaps, and freeze the survival substrate.
6. **People and society.** Only then begin population, settlements, production specialization, trade, institutions and related systems.

This order is a dependency chain, not a commitment to speculative systems beyond the next slice.

## Next major slice: Resource Acquisition v1

### Question it must answer

Can the walking player take useful material from the same world the simulation owns, and does that action remain true after time, LOD changes and save/load?

### Scope

Implement one end-to-end acquisition path rather than a generic future item framework.

The slice must provide:

- a player interaction action in the Godot walking scene;
- authoritative persistent player inventory/state owned behind the simulation boundary, not by presentation-only UI;
- queryable local resource availability at the player location;
- extraction/collection commands validated and applied by authoritative code;
- starter resource families sufficient to exercise different world semantics:
  - fresh water from hydrology;
  - one renewable plant-food source from ecology;
  - wood/fiber from living vegetation;
  - stone from geology/surface material;
  - one finite metal-bearing mineral resource, without introducing a full geochemistry model;
- depletion/removal from the corresponding authoritative source where material is actually taken;
- no negative source stocks and no duplicate creation through repeated commands;
- renewable sources recovering through their owning world process where applicable; finite mineral depletion remaining persistent;
- correct behavior across simulation LOD transitions;
- snapshot save/load and deterministic continuation including player inventory and depleted resource state;
- a minimal Godot interaction prompt and inventory readout so the complete path is playable rather than headless-only.

### Explicitly out of scope

Do not add crafting recipes, tools, buildings, hunger/thirst, NPCs, settlements, economy, a broad item database, equipment slots, rarity systems or procedural loot.

Do not modify natural-world equations merely to make acquisition numbers look convenient. If a gameplay source needs a new state variable, add the smallest domain-owned state with an explicit authority/LOD/snapshot contract.

### Definition of Done

Resource Acquisition v1 is done when automated tests and the playable client demonstrate all of the following:

1. the player can inspect and collect each starter resource through normal commands;
2. successful collection increases persistent inventory by exactly the amount removed from or granted by the authoritative source contract;
3. invalid, unavailable or over-large collection requests are rejected without partial hidden mutation;
4. resource/player state survives refine/coarsen and snapshot round trips under its declared semantics;
5. same seed + same player commands produce the same resulting authoritative state within the existing determinism contract;
6. the Godot client uses the same authoritative interaction path and displays the resulting inventory;
7. existing natural-world tests and the normal CI stability smoke remain green.

When these conditions pass, stop this slice and move to Crafting and Tool Use v1. Do not expand it into the later survival systems.
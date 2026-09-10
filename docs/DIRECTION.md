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

1. **Resource Acquisition v1 — COMPLETE (#60).** The walking player queries and gathers authoritative fresh water, food, wood, stone and metal ore into persistent inventory. See `RESOURCE_ACQUISITION.md`.
2. **Crafting and Tool Use v1 — COMPLETE (#62).** World-gathered wood and stone craft one persistent stone axe; the axe changes authoritative wood gathering from 1 kg to 3 kg per action.
3. **Shelter and Fire v1 — COMPLETE (#64).** World-gathered materials create a persistent campsite with a basic shelter and controlled fueled heat source.
4. **Resource Locality & World Interaction v2 — NEXT (#65).** Replace base-level regional gathering semantics with walking-scale authoritative interaction and make persistent campsites recognizable in the playable world.
5. **Survival Needs v1.** Thirst, hunger and exposure consume the resources/capabilities above; add the smallest renewable food loop needed to sustain one player.
6. **Solo-survival gate.** Run the complete player-visible loop, close remaining gameplay-blocking gaps, and freeze the survival substrate.
7. **People and society.** Only then begin population, settlements, production specialization, trade, institutions and related systems.

This order is a dependency chain, not a commitment to speculative systems beyond the next slice.

## Next major slice: Resource Locality & World Interaction v2 (#65)

### Question it must answer

Can the player walk to a spatially meaningful source or campsite, interact through the authoritative simulation boundary, leave, return and observe the same depleted resource or built structure independent of adaptive simulation LOD?

### Why this slice is required now

Resource Acquisition v1 intentionally proved authority and persistence using a base-level interaction region. That contract is no longer sufficient for a playable survival loop: `interaction_region()` currently resolves resource actions at `SimulationConfig::base_level` (level 4 in the playable composition), while rendered vegetation is decorative and has no resource identity. A raycast against those meshes would therefore only disguise the same regional extraction semantics.

Campsites already use fixed level-16 gameplay identity and authoritative persistent state, but the playable client only exposes the site under the player's current position. A built shelter or campfire needs a non-authoritative visual representation tied back to that persistent identity so the place can be recognized and revisited.

### Scope

Implement one walking-scale interaction path rather than a generic object or item framework.

The slice must provide:

- fixed gameplay-resolution resource interaction identity independent of adaptive simulation LOD;
- local resource availability that represents the gameplay interaction location rather than the complete level-4 source region;
- exact source debit and inventory credit for every successful gather, preserving the existing no-duplication invariant;
- persistent local depletion through movement, refine/coarsen and snapshot save/load;
- deterministic continuation for the same seed and command sequence;
- coherent renewal semantics for renewable food/wood and permanent depletion semantics for finite stone/ore;
- visual gatherable proxies only when they deterministically map to authoritative local source state and submit the same C++ gather command used by tests;
- a persistent Godot representation for built shelter/campfire anchored to authoritative campsite identity, without making scene nodes authoritative;
- the single `survival_interact` gameplay action as the entry point for contextual actions; do not return to one hotkey per resource/tool/building operation.

### Explicitly out of scope

Do not add a generic item database, inventory grid, free-placement construction system, procedural loot, durability, combat, NPCs, settlements, economy, broad natural-world recalibration or a presentation-owned resource system.

### Definition of Done

1. two distinct walking-scale resource locations inside one former level-4 interaction region can be queried independently;
2. gathering at location A changes A and carried inventory without silently consuming location B's local state;
3. resource depletion and revisiting survive LOD transitions and snapshot round trips;
4. water and material transfer accounting still closes exactly at the authoritative boundary;
5. any visible gatherable maps to the exact authoritative local source it depletes; decorative geometry alone cannot mint material;
6. a built campsite remains visually identifiable after the player walks away and returns while C++ remains the owner of shelter, fire and fuel state;
7. Godot exercises the same commands/state as automated tests through `survival_interact`;
8. existing natural-world, Resource Acquisition, Crafting, Shelter/Fire and determinism tests remain green.

When these conditions pass, stop this slice and move to Survival Needs v1. Do not pull needs or society-scale systems into it.

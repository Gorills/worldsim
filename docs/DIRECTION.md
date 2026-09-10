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
3. **Shelter and Fire v1 — NEXT (#63).** Consume gathered materials to create a persistent campsite with a basic shelter and controlled fueled heat source.
4. **Survival Needs v1.** Thirst, hunger and exposure consume the resources/capabilities above; add the smallest renewable food loop needed to sustain one player.
5. **Solo-survival gate.** Run the complete player-visible loop, close remaining gameplay-blocking gaps, and freeze the survival substrate.
6. **People and society.** Only then begin population, settlements, production specialization, trade, institutions and related systems.

This order is a dependency chain, not a commitment to speculative systems beyond the next slice.

## Next major slice: Shelter and Fire v1

### Question it must answer

Can the player turn world-gathered materials into a persistent place in the world, and can a controlled heat source consume carried fuel through simulation time without becoming a second wildfire model?

### Scope

Implement one campsite vertical path rather than a generic construction framework.

The slice must provide:

- fixed gameplay-resolution campsite identity independent of adaptive simulation LOD;
- one basic shelter built from `6 kg` wood + `2 kg` stone and requiring the existing stone axe;
- one controlled campfire built from `2 kg` stone;
- exact carried-wood transfer into campfire fuel;
- a fixed v1 burn rate of `0.25 kg wood` per simulated hour while lit;
- automatic extinguishing at zero fuel and no negative fuel state;
- rejection of duplicate, invalid, underfunded and clearly unsuitable placement requests before mutation;
- persistent campsite/fuel/lit state through refine/coarsen and snapshot save/load;
- deterministic continuation for the same seed and commands;
- Godot interaction and HUD state using the same authoritative C++ path as automated tests.

The natural `ecology.fire_*` fields remain wildfire authority. A controlled campfire must not directly write wildfire active/burned/emission state in this slice.

### Explicitly out of scope

Do not add a generic blueprint/building registry, free-placement gizmos, construction stages, durability, repair, storage containers, doors, architectural variants, cooking, smoke/particle simulation, wildfire ignition coupling, survival needs, NPCs, settlements or economy.

### Definition of Done

Shelter and Fire v1 is done when automated tests and the playable client demonstrate all of the following:

1. starting from empty player state, world resources are gathered, the stone axe is crafted, and one shelter plus one campfire are built without test-only material injection;
2. construction consumes exactly declared inventory inputs and invalid/duplicate requests leave authoritative state unchanged;
3. fuel transfer debits carried wood exactly once and credits campfire fuel exactly once, with over-large requests rejected atomically;
4. a lit campfire consumes `0.25 kg` fuel per simulated hour, extinguishes at zero and never creates negative fuel;
5. controlled campfire state remains separate from natural wildfire state;
6. campsite identity/state survives refine/coarsen and snapshot round trips;
7. same seed plus the same gather/craft/build/fuel/light commands produces the same authoritative continuation;
8. Godot exercises the same authoritative path and displays shelter/campfire state;
9. existing Resource Acquisition/Crafting tests, natural-world tests and normal CI stability smoke remain green.

When these conditions pass, stop this slice and move to Survival Needs v1. Do not expand it into later systems.
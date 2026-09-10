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

1. **Resource Acquisition v1 — COMPLETE (#60).** The walking player now queries and gathers authoritative fresh water, food, wood, stone and metal ore into persistent inventory. See `RESOURCE_ACQUISITION.md`.
2. **Crafting and Tool Use v1 — NEXT (#61).** Turn gathered materials into one functional persistent tool and make that tool change a real acquisition action.
3. **Shelter and Fire v1.** Place persistent structures/heat sources that consume gathered materials and interact with local conditions.
4. **Survival Needs v1.** Thirst, hunger and exposure consume the resources/capabilities above; add the smallest renewable food loop needed to sustain one player.
5. **Solo-survival gate.** Run the complete player-visible loop, close remaining gameplay-blocking gaps, and freeze the survival substrate.
6. **People and society.** Only then begin population, settlements, production specialization, trade, institutions and related systems.

This order is a dependency chain, not a commitment to speculative systems beyond the next slice.

## Next major slice: Crafting and Tool Use v1

### Question it must answer

Can the player turn materials gathered from the authoritative world into a persistent useful tool, and does that tool measurably change a real authoritative resource action?

### Scope

Implement one end-to-end tool path rather than a generic item, recipe or equipment framework.

The slice must provide:

- one `stone_axe` crafted from already-carried world resources;
- an atomic authoritative craft command that consumes exactly `1 kg` wood and `1 kg` stone;
- persistent non-spatial tool ownership behind the simulation boundary;
- rejection of insufficient or duplicate craft attempts without hidden mutation;
- one canonical gather action whose realized amount is determined by simulation state;
- bare-hand wood gathering of `1 kg` when available;
- stone-axe wood gathering of `3 kg` for the same action when available;
- exact debit of the authoritative wood source and exact credit of player inventory;
- correct refine/coarsen, snapshot and deterministic continuation behavior;
- Godot interaction using the same craft/gather boundary as the automated tests and showing tool state.

### Explicitly out of scope

Do not add a generic recipe registry, broad item database, equipment slots, durability, tool quality, animations, combat, a second tool, workbench, buildings, fire, hunger/thirst, NPCs, settlements or economy.

Do not retune resource/ecology/geology quantities to make crafting convenient. If the selected world location lacks inputs, the player must gather or travel rather than receive injected material.

### Definition of Done

Crafting and Tool Use v1 is done when automated tests and the playable client demonstrate all of the following:

1. starting from empty player state, the player gathers the required wood and stone from the world and crafts one stone axe without test-only inventory injection;
2. crafting consumes exactly the declared authoritative inputs and creates exactly one persistent tool;
3. invalid, insufficient and duplicate craft requests leave authoritative state unchanged;
4. a bare-hand wood gather realizes `1 kg`, while the same gather action with the axe realizes `3 kg`, with exact source debit and inventory credit;
5. tool, inventory and resource state survives refine/coarsen and snapshot round trips;
6. same seed plus the same gather/craft commands produces the same authoritative continuation under the existing determinism contract;
7. Godot exercises the same craft/gather boundary and displays tool state;
8. existing Resource Acquisition tests, natural-world tests and normal CI stability smoke remain green.

When these conditions pass, stop this slice and move to Shelter and Fire v1. Do not expand it into later survival systems.

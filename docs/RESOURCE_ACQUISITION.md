# Resource acquisition contract

`Resource Acquisition v1` is the first player-facing slice of the solo-survival milestone. It connects the walking player to authoritative persistent world resources without introducing crafting, buildings, needs, NPCs or economy.

## Authority boundary

Resource truth lives in the C++ simulation. Godot queries local availability and submits collection requests; UI counters and decorative meshes are never authoritative resource state.

The natural-world composition returned by `make_default_simulation()` remains unchanged and is still the frozen epoch-39 baseline. Playable survival uses `make_survival_simulation()`, which composes the same natural modules plus `ResourceModule`.

`PlayerInventoryStore` is a non-spatial authoritative state store. It follows the normal snapshot lifecycle and intentionally ignores simulation refine/coarsen because carried inventory is not attached to a simulation cell.

## Stable interaction region

A player interaction direction is resolved to the simulation base-level hierarchy region. Availability and collection then aggregate over whatever active leaves currently represent that stable region.

This prevents camera/focus refinement from changing how much material exists merely because the same physical hierarchy region is represented by more cells.

## Resource families

### Fresh water

Fresh water is measured in liters in player inventory. Availability comes from the existing authoritative hydrology surface reservoir plus groundwater in the interaction region.

Collection withdraws surface water first and groundwater second. One liter collected removes exactly `0.001 m3` from hydrology before one liter is credited to inventory. The projected hydrology fields are refreshed after a surface-reservoir withdrawal.

Water inventory is player gameplay state, not a second hydrology reservoir. Natural-world long-run validation continues to use `make_default_simulation()` and therefore does not include carried water. Survival-level accounting tests must include inventory when evaluating a complete player-plus-world water inventory.

### Plant food

`resources.plant_food_kg` is an extensive harvestable-food stock. It is not a second vegetation-carbon pool.

Its capacity is derived from current grass/shrub/tree carbon with small PFT-specific edible fractions. Harvesting depletes the harvestable stock. A daily resource-renewal pass relaxes that stock back toward the capacity implied by the current living vegetation, so dead or depleted vegetation cannot support unlimited food regeneration.

This is a reduced renewable-food abstraction for the solo-survival substrate. Species, fruits, seasons, nutrition and agriculture remain later work.

### Wood

`resources.wood_kg` is an extensive harvestable structural-biomass stock whose capacity is derived primarily from current tree carbon, with a smaller shrub contribution.

Harvesting depletes the harvestable stock. It recovers slowly toward the capacity supported by current living vegetation. The v1 stock deliberately represents harvestable material rather than directly removing structural plant carbon, because doing so would require extending the established carbon/nitrogen accounting boundary. If later gameplay requires tree-felling to alter ecological biomass immediately, that must be added as a bounded ecology/accounting extension with explicit carbon/nitrogen transfers.

### Stone

`resources.stone_kg` is a finite extensive gameplay resource initialized from represented land area and regolith depth. Collection permanently depletes the stock unless a later geological process explicitly produces new accessible stone.

The stock is separate from conserved crust/sediment mass because v1 does not yet model excavation geometry or return tailings into the geological mass budget. It is a finite accessible-resource inventory tied to geological state, not a new rock-physics model.

### Metal ore

`resources.metal_ore_kg` is one finite metal-bearing mineral family. Initial accessible stock is deterministic from world seed, represented land area and existing tectonic context (collision/rift/volcanic forcing).

This is intentionally not a geochemistry, lithology, grade-processing or multi-metal system. Its purpose is to establish persistent finite extraction semantics required by later tools/crafting.

## Transfer rules

Collection is all-or-nothing for the requested canonical amount:

1. validate resource kind, amount and direction;
2. compute authoritative availability for the stable interaction region;
3. reject an over-large request before mutation;
4. debit the source exactly once;
5. credit persistent player inventory by exactly the realized amount;
6. emit `gameplay.resource_collected`.

Resource source fields are non-negative. Repeated requests cannot create duplicate material because every successful request first debits the current authoritative source.

## LOD and persistence

The four ResourceModule stocks are extensive fields, so existing FieldStore refine/coarsen semantics split and sum them conservatively. Player inventory is global and unchanged by spatial LOD.

Both world resource fields and player inventory are serialized through the existing state-store snapshot framing. Same seed plus the same interactions must therefore reproduce the same authoritative continuation under the existing determinism contract.

## Playable Godot path

The walking scene uses `SurvivalSimulationNode`, a specialization of the existing adapter. The path is:

```text
player projected position
  -> SurvivalSimulationNode local query / collect call
  -> C++ ResourceModule + domain source
  -> persistent PlayerInventoryStore
  -> Godot inventory/local-resource readout
```

`Q` cycles the starter resource family and `E` requests one canonical unit while walking. The interaction is intentionally minimal; a targeting/raycast object system is not required to prove v1 authority semantics.

## Explicit non-goals

Resource Acquisition v1 does not include crafting recipes, tools, equipment, buildings, hunger/thirst, nutrition, agriculture, storage containers, resource-quality tiers, individual gatherable objects, procedural loot, NPCs, settlements or economy.

If a later slice requires stronger coupling between extraction and natural-domain conservation (for example tree felling that transfers plant C/N, or excavated ore that debits crust/sediment mass), extend the owning domain with an explicit accounted transfer. Do not silently mutate presentation objects or bypass the authoritative resource source.

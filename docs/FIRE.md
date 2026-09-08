# Wildfire v1 contract

> Soil carbon v1 subsequently raised the snapshot epoch to 20 and added
> conservative decomposition pools. Snow-albedo coupling v1 introduced epoch
> 21 and suppresses fire danger under projected snow cover. Fauna carbon
> accounting introduced epoch 22; the grazing-timestep correction subsequently
> raises the current combined-world epoch to 24.
> The Wildfire-v1 execution record below is retained as historical evidence for
> epoch 19.

WorldSim wildfire v1 is a reduced, daily disturbance process coupled to the
existing climate, hydrology, vegetation and fauna systems. It is designed to
exercise authoritative disturbance state, mixed-LOD propagation and explicit
fire-carbon accounting. It is **not** an operational fire-behaviour model or a
calibrated reconstruction of Earth's fire regime.

## Why this slice follows Climate v2

Climate v2 supplies persistent temperature, precipitation, relative humidity
and wind. Basin hydrology supplies root-zone water and inundation, while Flora
v1 supplies live plant carbon and litter. Wildfire is therefore the smallest
new domain that turns all three existing environmental systems into a visible
ecosystem disturbance and immediately changes the forage observed by fauna.
Human settlement, suppression and land-use fire remain out of scope until a
population domain exists.

The daily production order is:

```text
climate.surface -> hydrology.balance -> climate.surface_exchange
    -> geology.evolution
    -> ecology.soil -> ecology.vegetation
    -> ecology.fire -> ecology.fauna
```

## Authoritative state and LOD semantics

Wildfire uses ordinary registered fields so it participates in the existing
snapshot, C ABI, Godot field-introspection and LOD contracts:

- `ecology.fire_active_area_m2`: extensive authoritative area scheduled to
  burn on the next daily fire pass;
- `ecology.fire_active_fraction`: intensive projection of active area onto the
  current cell's land area;
- `ecology.fire_danger`: intensive dimensionless pre-burn diagnostic in
  `[0,1]`;
- `ecology.fire_burned_fraction`: intensive fraction burned by the most recent
  daily pass;
- `ecology.fire_burned_area_m2`: extensive cumulative area ledger;
- `ecology.fire_emitted_carbon_kg`: extensive cumulative fire-emission carbon
  ledger;
- `ecology.pyrogenic_carbon_kg`: extensive persistent char carbon stock.

Active area is extensive because independently averaging active fraction and
land fraction would not preserve their product when heterogeneous children
coarsen. `EcologyModule::on_spatial_cover_changed()` recomputes the displayed
fraction from the conserved area after each simulation-driven cover change.
The other three extensive quantities split and sum with the standard
field-store rules. Cumulative burned area can exceed current land area after
repeated fires.

Spread always starts from a frozen pre-transfer fire state. It queries
`WorldState::active_neighbors4()` and converts source burned fraction to an
explicit source burned area before distributing a small ignition area among
the active leaves representing the adjacent region. This avoids both same-day
multi-hop propagation and the invalid assumption that
`CubeSphereTopology::neighbors4()` returns active leaves. The region weights
are area-allocation weights, **not** inferred face lengths or a claim of
front-resolved fire geometry.

## Reduced process

For each terrestrial active cell, fuel availability is a bounded function of
live grass/shrub/tree carbon plus litter per effective land area. Combustibility
falls with root-zone wetness, relative humidity, recent precipitation,
inundation and freezing temperature. Wind magnitude increases within-cell
growth, while the projection of the local wind vector toward each neighbor
biases spread allocation.

Natural ignitions are rare deterministic draws from the repository's stateless
random contract. Their hazard scales with effective land area and fire danger,
so changing iteration order does not change the draw and refining a cell does
not assign the same per-cell hazard to every child. No human ignitions or
suppression are synthesized without an authoritative population state.

The burning fraction is bounded per daily pass. PFT-specific mortality and
combustion fractions remove plant carbon; uncombusted killed biomass becomes
litter. Burning litter is split between emitted carbon and persistent
pyrogenic carbon. Every fire pass therefore satisfies, to floating-point
roundoff:

```text
plant C before + litter C before + char C before + emitted C before
  = plant C after + litter C after + char C after + emitted C after
```

This identity covers fire transfers only. Soil carbon v1 separately closes
litter/soil decomposition against a cumulative respiration ledger. Fauna v3
also records its body-carbon transfers and respiration, but photosynthesis,
plant respiration and atmosphere/ocean exchange do not yet form a closed
planetary carbon cycle. `fire_emitted_carbon_kg` therefore
remains a cumulative destination ledger rather than a coupled atmospheric CO2
reservoir.

## Design references

- Thonicke et al. (2010), SPITFIRE:
  <https://doi.org/10.5194/bg-7-1991-2010>. The comparable process model gates
  ignition on fuel and dryness, treats fuel moisture as a control on fire
  danger/combustion, includes wind in spread, derives fuels from dynamic PFT
  state, and accounts for biomass-burning emissions. WorldSim adopts that
  process decomposition, not its calibrated equations or parameters.
- Community Terrestrial Systems Model, CLM5.0 technical note, section 2.24:
  <https://escomp.github.io/CTSM/release-clm5.0/tech_note/Fire/CLM50_Tech_Note_Fire.html>.
  CLM separately represents ignition, fuel availability, fuel moisture,
  burned area, PFT mortality, carbon transfer and emissions. Its documented
  lower/upper fuel thresholds and fitted human terms are not copied into this
  reduced fictional-world model.
- Repository contracts: `ARCHITECTURE.md`, `CLIMATE.md`, `HYDROLOGY.md` and
  `ECOLOGY_VALIDATION.md`.
- Godot 4.7 internationalization documentation:
  <https://docs.godotengine.org/en/4.7/tutorials/i18n/>. The laboratory keeps
  explicit English/Russian `tr()` keys for the curated fire fields instead of
  exposing untranslated technical names in its normal ecosystem view.

## Executed evidence — 2026-09-08

- GCC 13.3 RelWithDebInfo build and the complete CTest suite passed: **11/11**
  in 58.84 seconds on this machine.
- `worldsim_fire_tests` passed controlled fire-carbon closure, no-fuel
  extinction, wet-weather suppression, deterministic natural ignition, PFT
  aggregate consistency, coarse-to-fine spread, extensive ledger restriction
  and epoch-19 snapshot round-trip.
- ASan + UBSan + float-cast-overflow builds passed `worldsim_fire_tests`,
  `worldsim_sanitizer_smoke` and all 14 `worldsim_audit_tests` scenarios with
  leak detection disabled for the tracing sandbox. LeakSanitizer remains
  **NOT VERIFIED** for the same environment limitation recorded by the audit.
- Godot **4.7.2.stable.mono.official.ed1daf0bf** rebuilt against the pinned
  godot-cpp **10.0.0-rc2** / API 4.7 target. `ci_smoke.gd` discovered 62 fields,
  and the Russian `ci_simulation_lab.gd` run rendered its 512 x 256 map,
  discovered the fire fields and completed at tick 48.
- The gettext catalogs passed `msgfmt --check`; their existing metadata-only
  warnings remain unchanged.
- Interactive performance and visual layout on target player hardware are
  **NOT VERIFIED**.

## Explicitly unsupported claims

Wildfire v1 does not provide sub-cell fire fronts, flame intensity, crown-fire
physics, fuel-size/moisture classes, topographic rate-of-spread, lightning
climatology, smoke or trace-gas chemistry, peat fire, fire suppression,
anthropogenic ignition, fire-adapted plant traits, fauna injury, observation
calibration, or target-hardware performance evidence. Natural ignition and
spread coefficients are bounded simulation parameters, not Earth estimates.

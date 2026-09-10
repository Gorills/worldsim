# Wildfire v1 contract

> Soil carbon v1 subsequently raised the snapshot epoch to 20 and added
> conservative decomposition pools. Snow-albedo coupling v1 introduced epoch
> 21 and suppresses fire danger under projected snow cover. Fauna carbon
> accounting introduced epoch 22; later timebase, LOD, climate, carbon,
> nitrogen and geography corrections advanced the combined-world epoch through
> 36. Fixed-support natural-fire ignition raised that compatibility epoch to 37; later authoritative-domain changes advance it further.
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
random contract. Their hazard scales with effective land area and fire danger.
The stochastic process is anchored to fixed cube-sphere level-4 regions: active
cells contribute hazard to those regions, each region performs one ignition and
size draw, and adaptively refined regions deterministically assign an ignition
to one active descendant in proportion to its hazard contribution. Coarse
L1-L3 worlds therefore sample the same level-4 process instead of inventing a
new per-cell random process, while a uniform level-4 world retains the former
ignition/size stream keys and calibration. No human ignitions or suppression
are synthesized without an authoritative population state.

The burning fraction is bounded by a 25% per-day ceiling converted to the
current integration window with elapsed simulation time. A half-day fire pass
therefore caps burning at 12.5%, while the default one-day pass retains the
existing 25% calibration. The fraction of burned area retained as active fire is
also calibrated per day and is converted to the current integration window as
`daily_persistence ^ dt_days`; half-day passes therefore apply the square root
of the one-day persistence instead of applying the full daily decay twice.
PFT-specific mortality and
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

## Resolution-consistent natural ignition — 2026-09-09

The two-year L2/L3 stability matrix exposed a large stochastic fire artifact:
for seed 42 the former operator produced zero annual burned fraction at level 2
and about 21.6% at level 3. Although the ignition hazard already scaled with
land area, both the Bernoulli draw and ignition-size draw were keyed by the
current active cell id, so changing spatial resolution changed the random
process itself.

A regression-only head confirmed the defect on a dry, fully fueled fixture:
for seed 0 the first natural ignition occurred on day 9 at L2 and day 8 at L3,
with different burned-area magnitudes. The corrected operator uses fixed level-4
ignition regions and the same reference draw for L2, L3 and L4; an additional
L4/L5 fixture verifies that adaptive refinement does not duplicate or move the
first ignition event.

On the standard two-year coupled seeds `0,42,999`, levels `2,3` smoke, the
largest absolute annual-burn mismatch falls from about **21.64** to **10.30
percentage points of represented land**. Seed 0 falls from about **2.08 pp** to
**0.0056 pp**, and seed 999 becomes zero at both levels in this short window.
Relative burned-fraction error remains a poor diagnostic when both values are
near zero.

Downstream effects are path-dependent rather than uniformly monotonic:
worst-case NPP-density delta improves from 20.89% to 16.78%, fauna-carbon
density remains about 7.41%, and precipitation is essentially unchanged.
Individual seeds can move in either direction because synchronizing ignition
changes which disturbance history each world follows. This slice is therefore
accepted for stochastic correctness and reduced absolute fire-resolution error,
not as fire calibration.

The changed stochastic continuation advances the combined snapshot epoch from
36 to 37 even though field/store binary layouts are unchanged.

## Nitrogen coupling added after Wildfire v1

Nitrogen cycle v1 tracks the N associated with vegetation and litter removed by
fire. N returned with uncombusted mortality stays in litter; the remainder is
split between a reduced mineral-ash return and cumulative
`ecology.fire_emitted_nitrogen_kg` boundary ledger. This closes tracked
nitrogen through fire without claiming smoke chemistry, NOx speciation or
atmospheric nitrogen transport. The authoritative contract is in
`NITROGEN_CYCLE.md`.

## Explicitly unsupported claims

Wildfire v1 does not provide sub-cell fire fronts, flame intensity, crown-fire
physics, fuel-size/moisture classes, topographic rate-of-spread, lightning
climatology, smoke or trace-gas chemistry, peat fire, fire suppression,
anthropogenic ignition, fire-adapted plant traits, fauna injury, observation
calibration, or target-hardware performance evidence. Natural ignition and
spread coefficients are bounded simulation parameters, not Earth estimates.

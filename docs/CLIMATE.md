# Climate v2 — reduced coupled energy and moisture model

> Snow-albedo coupling v1 introduced snapshot epoch 21 and makes hydrology's
> snow stock affect land shortwave absorption,
> vegetation exposure and fire danger. Its current contract is documented in
> `SNOW_ALBEDO.md`; the Climate-v2 execution record below remains historical
> evidence for epoch 18. Fauna carbon accounting introduced epoch 22; the
> grazing-timestep correction introduced epoch 23, the magic forcing
> timebase correction introduced epoch 24, and the wildfire burn-cap
> timebase correction introduced epoch 25, the active-fire persistence correction introduced epoch 26, and LOD-invariant field-command routing introduced epoch 27, the parent-consistent LOD hysteresis correction raised the combined-world epoch to 28,
the resolution-aware lateral heat-transport correction raised it to 29, Carbon cycle v1 raised it to 30, Nitrogen cycle v1 raised it to 31, Planetary nitrogen v2 raised it to 32, fauna nitrogen stoichiometry raised it to 33, area-integrated coastal geography raised it to 34, and fixed-support climate orography raises the current combined-world epoch to 35. Carbon-cycle state and assumptions are documented separately in `CARBON_CYCLE.md`; nitrogen state is documented in `NITROGEN_CYCLE.md`.

## Decision and verified baseline (2026-09-08, before implementation)

The existing `climate.surface` system is cell-local. It diagnoses temperature
from latitude, elevation, a seasonal sine and a stochastic anomaly, then
diagnoses precipitation from latitude bands, local elevation and land fraction.
It has no transported atmospheric-water stock, no ocean heat capacity and no
surface/atmosphere return path. `hydrology.balance` therefore treats diagnosed
precipitation as an external water input and evaporation/ocean discharge as
external outputs. The basin solver itself closes that deliberately open land
budget, but the planet does not have a closed water cycle.

This slice replaces only that verified failure of causality. It does not add a
three-dimensional atmosphere, solved momentum, clouds, ocean currents, dynamic
sea level or scientific Earth calibration. Geography remains authoritative in
the kernel, rendering LOD remains separate, and Godot remains a client.

Affected contracts are climate field registration, hydrology evaporation and
ocean-export coupling, scheduler ordering/resources, adaptive-cover projection,
snapshots, the C ABI's generic field discovery, Godot laboratory diagnostics,
and seasonal/audit integration tests. The new authoritative state advances the
global snapshot epoch from 17 to 18 under the existing strict policy.

## References and modeling boundary

- C++20 WG21 N4861 for deterministic container traversal and explicit binary
  primitives: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/n4861.pdf
- UVic ESCM 2.10 uses a two-dimensional atmospheric energy-moisture balance
  model with prescribed winds for heat/moisture advection and coupled surface
  heat/water fluxes: https://gmd.copernicus.org/articles/13/4183/2020/
- ZEMBA v1.0 separates land and ocean thermal response and uses a passive ocean
  mixed layer where a dynamic ocean is not represented:
  https://gmd.copernicus.org/articles/18/2479/2025/
- WAM2layers v3 documents a conservative upstream/donor-cell atmospheric-water
  transport scheme and explicit evaporation/precipitation source/sink budget:
  https://gmd.copernicus.org/articles/18/4335/2025/
- GREB hydrological-cycle development demonstrates that moisture advection,
  diffusion and convergence are required even in a reduced climate model:
  https://gmd.copernicus.org/articles/12/425/2019/
- Godot 4.7 thread-safety guidance states that the active scene tree must not be
  manipulated from worker threads. Laboratory fast-forward is therefore made
  cooperative across frames rather than moving scene operations off-thread:
  https://docs.godotengine.org/en/4.7/tutorials/performance/thread_safe_apis.html

These are comparable modeling and implementation practices, not dependencies
and not evidence that WorldSim reproduces their fidelity.

## Representation and LOD

`ClimateStore` owns a persistent **uniform reference cover** at the initial
simulation base level, analogous to the domain-resolution choice already made
by `HydrologyStore`. Each reference node owns land and ocean surface
temperature, atmospheric water volume and the stochastic weather anomaly. The same store also owns global atmospheric and ocean carbon reservoirs; those are scalar planetary stocks rather than per-node copies.
Reference geometry fixes area, initial base-cover elevation and thermal
land/ocean partition; derived solar, wind, humidity, precipitation and exchange
diagnostics are projected to active fields.

Focus refinement never creates climate reservoirs or changes the transport
graph. An active leaf maps to its base-level ancestor. The reference geometry
is deliberately not reconstructed from focus-refined fields: otherwise a
presentation-driven coastline reclassification changes heat capacity and
creates a false energy impulse. Current local elevation still affects projected
surface temperature through the lapse-rate term. Explicit slow climate-geometry
coupling for physical geological evolution is outside this slice and must use a
separate LOD-invariant contract.

Atmospheric transport links exist only between same-level nodes in this fixed
uniform graph. `neighbors4()` is used to identify topology there, never as an
adaptive active-neighbor query. Each link stores its own center distance,
approximate interface length and tangent orientation. Area weights are not
reinterpreted as face lengths.

## Energy closure

Daily-mean top-of-atmosphere insolation follows latitude, axial tilt and orbital
phase, including polar night/day. Land and ocean are separate heat reservoirs:

```text
C dT/dt = absorbed shortwave - linearized outgoing longwave
          + CO2 radiative forcing
          + conservative horizontal heat exchange
```

The reduced constants (effective albedo, linear outgoing-longwave response,
land heat capacity, mixed-layer depth and effective lateral heat diffusivity)
are engineering parameters. Pairwise heat exchange uses a finite-volume
conductance from the shared-interface length, center distance and symmetric
areal heat capacity. Each link solves its two-reservoir relaxation analytically,
so exchange is bounded by pair equilibrium and exactly antisymmetric before
floating-point roundoff. This replaces the former fixed per-neighbor relaxation
fraction, whose effective diffusivity changed with cell size. Radiation is an external source/sink recorded in a cumulative energy ledger;
Carbon cycle v1 adds its logarithmic CO2 term to that same ledger as
`co2_forcing_j`. Horizontal transport is internal and must not change total
stored heat.

The existing magic temperature anomaly remains an optional additive forcing at
projection time. A geography+climate+hydrology world must build and run without
`MagicModule`, correcting the previous mismatch with the documented optional
module contract.

## Water closure and scheduler split

The default coupled order is:

```text
magic.flux (optional)
    -> climate.surface
    -> hydrology.balance
    -> climate.surface_exchange
    -> geology.evolution
    -> ecology.soil -> ecology.vegetation -> ecology.fauna
```

`climate.surface` performs simultaneous donor-limited moisture advection and
diffusion, then removes donor-limited condensation as precipitation. The
precipitation field is an intensive rate projected over the whole reference
node. Hydrology consumes its active land-area share in the same tick; the
exchange system area-integrates that same current land mask and assigns the
remainder to the ocean, so the partition closes without changing thermal
geometry when focus LOD changes.

`climate.surface_exchange` then:

- adds actual land evapotranspiration and open-water evaporation reported by
  hydrology back to atmospheric water;
- adds ocean evaporation to atmospheric water and debits the ocean reservoir;
- returns the ocean-area share of precipitation to that reservoir;
- transfers hydrology's pending terrestrial ocean export into the reservoir.

This operator split makes newly evaporated water available to transport on the
next base tick. No iterative same-tick atmosphere/land solve is implied. In the
default coupled world the authoritative inventory is:

```text
ocean + atmosphere + snow + soil + groundwater + routed surface water
```

All internal transfers are donor-limited. Cumulative component ledgers remain
diagnostics and are not counted as stocks.

## Reduced circulation and precipitation

Version 2 prescribes a smooth Earth-like zonal circulation from latitude, with
a bounded seasonal/planetary-wave meridional component. It does not solve air
momentum. Link-normal velocity advects column water with a conservative upstream
scheme; a symmetric diffusion term represents unresolved mixing. Simultaneous
outgoing requests are jointly limited by each donor.

Column saturation capacity is a bounded exponential function of temperature.
Condensation removes supersaturation plus a humidity-dependent background
fraction. Moist inflow climbing to a higher reference elevation contributes a
bounded orographic condensation request, producing a causal windward/leeward
contrast without claiming cloud microphysics.

## Validation contract

The slice is complete only when executable tests demonstrate:

- the closed climate-only water cycle conserves its inventory and transport
  never creates negative node stocks across the uniform cube-sphere graph;
- the annual radiation ledger closes, so internal heat transport cannot change
  total stored energy;
- donor-limited precipitation never overdraws atmospheric water;
- a controlled mountain barrier produces greater windward than leeward
  precipitation under the same latitude-scale forcing;
- ocean temperature has lower seasonal amplitude and later response than land
  temperature at comparable latitude;
- the coupled planet-water inventory closes through precipitation,
  evaporation and ocean export;
- fixed-geography climate reference state is unchanged by focus-only LOD
  history;
- snapshot round-trip and continuation preserve a mid-season climate state;
- multi-year, multi-seed states and every projected field remain finite and
  bounded;
- the Godot laboratory discovers and renders the new fields, and long advances
  are divided across frames so the scene remains responsive.

Broad climatology diagnostics are warnings until explicit calibration targets
and reference datasets are adopted. Passing conservation and process tests is
not a claim of modern-Earth accuracy.

## Resolution-aware heat transport — 2026-09-09

The stability matrix added after Climate v2 exposed base-resolution sensitivity
that focus-LOD invariance tests could not detect: those tests correctly prove
that refining the active cover does not change the fixed climate reference
grid, but they do not compare simulations constructed with different reference
levels.

A flat 50/50 land/ocean fixture run for two years measured a 3.521772 K mean
absolute temperature difference between level 2 and area-aggregated level 3
under the former fixed neighbor-relaxation operator. After switching lateral
heat exchange to geometry-scaled finite-volume conductance, the same deterministic
fixture measures 0.261492 K. The regression gate is 0.35 K; the pre-fix operator
therefore fails it by an order of magnitude.

On the full coupled seeds 0, 42 and 999, the two-year L2/L3 maximum NPP-density
delta fell from 49.6% to 11.7%, vegetation-density delta from 45.2% to 10.3%,
and fauna-carbon-ratio delta from 8.8% to 1.7%. Geography still contributes up
to 18.9% represented-land-area difference for these seeds, so this correction
does not claim complete spatial convergence.

Because the transport operator changes future authoritative state from the same
snapshot bytes, the combined-world snapshot compatibility epoch advanced from
28 to 29 even though that correction kept the ClimateStore binary layout at
version 2. Carbon cycle v1 subsequently changes the store layout to version 3
and advances the combined-world epoch to 30. Nitrogen cycle v1 subsequently
changes the field schema and authoritative ecology continuation state, raising
the combined-world epoch to 31. Planetary nitrogen v2 subsequently adds a
separate global nitrogen store and new field schema, raising the combined-world
epoch to 32 without changing ClimateStore v3. Fauna nitrogen stoichiometry then
raises the epoch to 33 and area-integrated coastal geography to 34. The
fixed-support orographic reference documented below changes ClimateStore to v4
and raises the current combined-world epoch to 35.

## Resolution-consistent orographic reference — 2026-09-09

After coastal land area was made resolution-consistent, the standard two-year
level-2/3 matrix still showed a maximum 21.33% mean-land-precipitation delta.
The climate moisture operator diagnosed uplift from neighboring
`ClimateNode.mean_elevation_m` values, while those values came from the
representative cell-center elevation. L2 and L3 therefore described the same
generated mountain system with unrelated coarse orographic samples.

Geography now exposes `geography.reference_elevation_m`. Below level 4 it is
the area-weighted mean of the same fixed level-4 flexed initial surface used as
the common reference support; level 4 and finer use their direct reference
cell. Climate stores that value separately as `orographic_elevation_m` and
uses it only for uplift condensation. The existing center elevation remains
the thermal lapse-rate reference and the local projected temperature contract.
This separation is intentional: an earlier implementation reused the
area-mean reference for thermal initialization and improved temperature
convergence while worsening several downstream ecological diagnostics.

The approach follows established model-orography practice: ECMWF derives mean
model-grid orography by aggregating a substantially finer elevation dataset and
treats unresolved terrain separately rather than classifying a whole coarse
grid box from one point sample.

- ECMWF, *Impact of orographic drag on forecast skill*:
  https://www.ecmwf.int/en/newsletter/150/meteorology/impact-orographic-drag-forecast-skill
- ECMWF Forecast User Guide, *Model orography*:
  https://confluence.ecmwf.int/pages/viewpage.action?pageId=673552287

A regression constructs geography+climate worlds for seeds 0, 42 and 999 at
levels 2 and 3 and requires every L2 climate orographic reference elevation to
equal the area-weighted aggregate of its L3 children to within 1e-6 m. The
regression-only commit failed on the former center-sampled operator.

On the final bounded implementation, the two-year coupled seeds
`0,42,999` L2/L3 smoke changes the maximum adjacent-level deltas relative to
the coastal-geography main baseline as follows:

- mean land precipitation: 21.33% -> **19.25%**;
- total NPP: 26.01% -> **20.77%**;
- NPP per represented land area: 26.39% -> **21.19%**;
- mean fertility: 24.45% -> **16.62%**;
- mineral-N density: 25.37% -> **23.78%**;
- fauna carbon density: 7.65% -> **7.41%**.

Vegetation density changes from 23.34% to 23.55% and mean land temperature
from 0.877% to 0.898%; those small regressions are retained rather than
changing unrelated thermal/ecology semantics to optimize a two-year matrix.
A separate attempted compositional rainout-law change was also rejected after
it worsened precipitation convergence to 25.75% and NPP-density convergence to
34.77%.

The new geography field changes the field schema, and the additional
ClimateStore orographic reference changes that store layout from v3 to v4.
The combined-world snapshot epoch therefore advances from 34 to 35. Version 34
is rejected rather than silently continuing with the former orographic
semantics.

## Executed evidence — 2026-09-08

- RelWithDebInfo build and the complete CTest suite passed: **10/10**.
- `worldsim_climate_tests` passed the optional-magic composition, annual
  energy/water accounting, seasonal inertia/lag, orographic precipitation,
  LOD-reference, malformed-store and snapshot continuation scenarios.
- ASan + UBSan + float-cast-overflow builds passed `worldsim_climate_tests`,
  `worldsim_sanitizer_smoke` and the annual `worldsim_audit_tests`, with leak
  detection disabled for the tracing sandbox as in the preceding audit.
- Godot **4.7.2** rebuilt the GDExtension and passed `ci_smoke.gd` plus
  `ci_simulation_lab.gd`: 55 fields were discovered, the 512 x 256 map rendered,
  and the test observed a 720-hour request stop after its first 24-hour chunk.
- The seed-42, level-2 diagnostic advanced **730 days** while alternating focus
  every 90 days. Maximum relative planet-water residual was **2.06951e-10** and
  maximum relative surface-energy residual was **8.98413e-13**.
- Its final reference map reported land temperature 248.129..304.096 K, ocean
  temperature 260.793..294.109 K, relative humidity 0.178543..0.950362 and
  precipitation 0.0186217..3.9686 mm/day. These are broad finite-state
  diagnostics, not observational calibration targets.
- The same 30-day focused CLI scenario used for the pre-slice baseline changed
  from 12.08 s / 8,832 KiB maximum RSS to 12.94 s / 9,284 KiB in this
  environment after replacing repeated map lookups with one dense-row lookup
  per active cell. This single-run measurement is a regression guard, not a
  target-hardware performance claim.

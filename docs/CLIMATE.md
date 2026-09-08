# Climate v2 — reduced coupled energy and moisture model

> Snow-albedo coupling v1 subsequently raises the current snapshot epoch to 21
> and makes hydrology's snow stock affect land shortwave absorption,
> vegetation exposure and fire danger. Its current contract is documented in
> `SNOW_ALBEDO.md`; the Climate-v2 execution record below remains historical
> evidence for epoch 18.

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
temperature, atmospheric water volume and the stochastic weather anomaly.
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
          + conservative horizontal heat exchange
```

The reduced constants (effective albedo, linear outgoing-longwave response,
land heat capacity and mixed-layer depth) are engineering parameters. Pairwise
heat exchange is simultaneous, relaxes toward the two-reservoir equilibrium and
is exactly antisymmetric before floating-point roundoff. Radiation is an
external source/sink recorded in a cumulative energy ledger; horizontal
transport is internal and must not change total stored heat.

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

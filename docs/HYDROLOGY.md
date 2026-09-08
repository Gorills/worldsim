# Basin hydrology — design and implementation contract

## Decision and references (2026-09-08, before implementation)

Replace the ecology water bucket with coupled snow, root-zone, shallow
groundwater and surface storage. The current `ecology.hydrology` runs every six
ticks, discards overflow into a rate diagnostic, and geology instantaneously
accumulates that rate down a strictly downhill graph. Soil and vegetation read
the root-zone stock; geology and soil consume runoff at a different cadence.
The affected contracts are module registration, field semantics, scheduler
access/order, geology discharge, ecology moisture/turnover, snapshots, generic
field consumers, and the existing water-budget/continuation tests.

Sources read for this decision:

- C++20 WG21 N4861: standard containers, deterministic ordering, explicit binary
  primitives; https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/n4861.pdf
- PCR-GLOBWB **2** (2018), sections 2.3.1–2.3.4: snow/rain partition, soil and
  groundwater stores, capillary rise, baseflow and surface routing as coupled
  modules. https://gmd.copernicus.org/articles/11/2429/2018/
- Landlab **2.9.1**, OverlandFlow manual: stored water depth, free-surface-driven
  link discharge and internal timesteps.
  https://landlab.readthedocs.io/en/v2.9.1/user_guide/overland_flow_user_guide.html
- Hunter et al. (2005), storage-cell inundation: continuity in each reservoir and
  the higher bed elevation as the connection sill.
  https://doi.org/10.1016/j.advwatres.2005.03.007
- Landlab DepressionFinderAndRouter: depressions, outlets, and distinction
  between individual pits and connected lakes.
  https://landlab.readthedocs.io/en/latest/generated/api/landlab.components.depression_finder.lake_mapper.html
- AMReX AmrCore, flux registers: conservation requires agreement of integrated
  transfers, not just restriction of stocks.
  https://amrex-codes.github.io/amrex/docs_html/AmrCore.html

These are modeling precedents, not new dependencies. WorldSim remains C++20
and does not implement Landlab's shallow-water equations or AMReX's solver.
No Godot API changes are needed: descriptors and field arrays already expose
new diagnostics. No calibrated flow velocity or performance claim is made.

## Representation and LOD

`HydrologyStore` owns a persistent storage-cell graph on the **initial uniform
cover** (the configured base level). It stores bed elevations, land areas,
surface volumes and integrated flux history. Its node IDs survive focus LOD.
This is an explicit domain resolution: camera refinement improves local land
processes, not river geometry. It avoids destroying a sill or changing river
travel time merely because active cells merge. It is not an adaptive hydraulic
solver or a fine-resolution river network hidden inside every coarse cell.

Snow water equivalent, soil water and groundwater are extensive active-cell
fields, split/summed by the existing field store. Fine-cell runoff aggregates
to its reference node exactly once; surface infiltration and diagnostics
project back with conservative area weights. Reference-grid neighbor queries
use `neighbors4()` only on that uniform grid. Region area weights are never
interpreted as shared face lengths.

Physical geology updates contribute area-weighted **bed elevation increments**
to reference nodes. LOD surface reconstruction contributes none. Rebuilding
drainage diagnostics changes geometry but retains every surface volume, so
uplift, erosion and changing sills can redirect existing water. Surface water
reaching an ocean node exits through an explicitly accumulated ocean budget.

## Processes

Hydrology executes every climate tick, sampling current forcing, with bounded
internal substeps. All transfers are capped by the donor's available stock.

- Precipitation is rain or snow across a finite freezing transition; positive
  degree days melt the persistent snow store.
- Rain/melt infiltrate subject to regolith-dependent rate and root-zone
  capacity; excess becomes surface runoff. Existing soil above capacity also
  spills conservatively when capacity changes.
- Soil above field capacity percolates into shallow groundwater. A linear
  reservoir releases baseflow; capillary return is limited by soil deficit,
  groundwater availability and an exchange rate.
- Temperature-based potential evaporation is apportioned to bare soil and
  biomass-dependent transpiration. Actual withdrawal cannot exceed soil water.
  Surface evaporation uses the current wetted area. Both are external outputs.
- Standing surface water can infiltrate into unsaturated local soil. Persistent
  inundation suppresses terrestrial growth and adds mortality to litter.

Each surface node has a monotone, invertible subgrid volume/height curve: a
small channel area broadens over a fixed relief scale into the full land area.
This is an engineering hypsometry, not inferred fine terrain. Neighbor links
carry water only above their common sill `max(bed_a, bed_b)` and toward lower
free surface. Transfers relax toward pair equilibrium over a travel time based
on node distance. Joint donor limiting and simultaneous application prevent
negative water and iteration-order multi-hop transport. Internal substeps
resolve routing; coarse/fine focus changes do not change its travel time.

The same stored surface water therefore provides river recession, backwater,
lake filling, overflow, connection of adjacent inundated cells and drying.
Priority-flood drainage diagnostics identify potential ocean outlets and spill
heights without filling the authoritative bed or teleporting stored water.
On an all-land planet, the lowest node is a closed sink, not an invented ocean.

## Coupling and accounting

`climate.surface -> hydrology.balance -> climate.surface_exchange ->
geology.evolution -> ecology.soil -> ecology.vegetation -> ecology.fauna`
is the serial, declared dependency chain (magic may precede climate). Hydrology owns all water withdrawals, including
transpiration, so vegetation does not independently spend that stock again.
Geology consumes accumulated routed volume over its elapsed interval instead
of multiplying the final instantaneous runoff sample by an entire day. Soil
leaching likewise uses integrated local drainage.

The land-water budget is:

`snow + soil + groundwater + surface = initial + precipitation - evaporation
- ocean export + explicitly imposed external water`.

Derived field arrays are projections, not a second surface-water inventory.
They include surface volume/depth/level, flooded fraction, inundation duration,
river discharge, baseflow, snowmelt, evapotranspiration, basin/outlet identity
and spill elevation. Core state and integrated histories are snapshotted;
adjacency and drainage caches rebuild deterministically. Climate consumes the
actual evaporation and terrestrial ocean-export window after hydrology, closing
the default world's planet-water inventory. Epoch **18** rejects older
snapshots under the repository's existing strict policy.

## Completion evidence

Required process scenarios: winter accumulation and melt; recharge and
baseflow recession; delayed pulse at a downstream reach; closed depression,
spill and communicating lakes; wet-soil feedback and flood mortality;
conservation with evaporation/ocean exports; physical sill changes; uniform vs
mixed-cover exchange; snapshot continuation including a partial geology
window; smaller-timestep comparison; multiple seasonal cycles. A headless
diagnostic executable must export map fields and basin histories so results
are inspectable beyond test assertions.

Exclusions: dynamic sea-level feedback, aquifer
pressure PDEs, calibrated river cross-sections, resolved flood velocities,
glacial mechanics, aquatic species, human water use and a dynamic water mesh.

## Implemented closure parameters

All values below are **engineering scales**, not Earth calibration:

| Closure | Value |
| --- | --- |
| Rain fraction | linear from 272.15 to 274.15 K |
| Degree-day melt | 0.003 m water / K / day above 273.15 K |
| Soil capacity | `0.02 + 0.28 (1 - exp(-regolith/0.75))` m |
| Infiltration limit | `0.002 + 0.025 (1 - exp(-regolith/0.75))` m/day |
| Field capacity / capillary target | 65% / 45% of root-zone capacity |
| Percolation / groundwater recession | exponential, 4 / 45 days |
| Capillary rise / surface reinfiltration | at most 0.001 / 0.004 m/day (latter weighted by inundation) |
| Potential evaporation | `max(0, T-258) * 0.000035` m/day |
| Canopy transpiration factor | `1-exp(-vegetation carbon density/2)` |
| Hypsometry | 1% channel strip widening linearly to full land area over 2 m |
| Routing travel time | center distance / nominal 1 m/s, at least 0.25 day |
| Internal step | at most 0.0625 day; one call accepts at most 366 days |
| Flood history | accumulates above 10% inundation, otherwise decays over 2 days |
| Additional plant turnover | `0.02 * flooded_fraction * (1-exp(-inundation_days/5))` per day |

For depth `h <= H=2 m`, surface volume is
`A * [f*h + (1-f)*h*h/(2H)]`, with `f=0.01`; above H the derivative is A.
Pair equilibrium is solved against these monotone curves. Each link moves a
fraction `1-exp(-dt/travel_time)` of the equilibrium exchange, additionally
limited by volume above the connection sill. All requests use the same initial
substep state and debit/credit a single transfer. This is reservoir relaxation,
not a Manning/shallow-water or momentum-conserving solver.

Potential drainage basin IDs denote terminal reference outlets; lake IDs
identify currently connected ponded surface components. IDs are deterministic
reference-node indices, not globally permanent named lake entities. Physical
mergers, drying and rerouting may change them. Projected discharge is the
outgoing volume rate allocated by reference area; it is not a measured flow
through every fine-cell face. Maps use the kernel's Z-polar latitude convention.

`HydrologyStore::add_surface_water` is an explicit external domain input.
Budget closure around such a call must include the injected volume; cumulative
precipitation excludes it. Surface projections should not be commanded as if
they were independent authoritative stocks. Changes to land area preserve
surface volume and recalculate depth; water on newly submerged reference nodes
is exported on the next hydrology pass.

## Reproduction

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./out/dev/worldsim_hydrology_dump --days 730 --level 2 --adaptive --output out/hydrology-diagnostics
# Optional plotting dependency, outside the C++ runtime:
python3 scripts/plot_hydrology.py out/hydrology-diagnostics
```

The dump writes `history.csv`, `nodes.csv`, `map.csv` and `summary.json`.
`history.csv` includes all global stores, cumulative external fluxes, budget
residual, selected basin storage, daily mean reach discharge and water level.
The selected basin can change if physical geography changes its outlet; this
is a diagnostic selection, not a fixed material control volume. The plotting
script writes `history.png` and `maps.png` without interpolation of cell state.

## Executed validation — 2026-09-08

- GCC 13.3 / C++20 / CMake dev: built with repository warning flags.
- Full CTest: **8/8 passed**. The dedicated hydrology executable covers seven
  process groups: snow/recharge/recession, delayed routing and timestep
  comparison, communicating lakes and spill/breach, mixed-cover exchange and
  integrated-window continuation, invalid state/flood feedback, physical
  geology versus focus changes, and two seasonal cycles.
- The existing soil-storage regression now compares accumulated drainage over
  its six-hour experiment, since the final hourly rate does not represent the
  entire storm. Its original thin-versus-deep-regolith failure mode is retained.
- ASan + UBSan + float-cast-overflow: hydrology process tests and audit tests
  (including the three-seed annual full-world continuation) passed with
  halt-on-error. Leak detection remains disabled for the tracing sandbox, as
  documented in `AUDIT_2026-09-08.md`.
- Full default simulation, seed 42, **730 days**, reference level 2 (96 nodes),
  alternating focus refinement/coarsening every 90 days: maximum relative
  global water-budget residual **2.70362e-14**. This is numerical accounting
  evidence, not calibration of the climate or river parameters.
- Inspected generated `history.png` and `maps.png`; maps show the actual coarse
  reference resolution and history includes delayed storage/flow response.
  Plots generated with Matplotlib 3.11.1 and NumPy 2.5.3 in a temporary Python
  environment, not a new C++ runtime dependency.
- Godot 4.7.2 / godot-cpp 10.0.0-rc2 targeting API 4.7: rebuilt native adapter;
  `ci_smoke.gd` passed, including new finite/nonnegative/aligned hydrology field
  checks. The existing terrain-only render-packet negative check emits its
  expected handled error.
- Basin hydrology's epoch **17** roundtrip and continuation passed; Climate v2
  introduced epoch **18**, and the current Wildfire-v1 epoch is **19**.
  `git diff --check` passed.

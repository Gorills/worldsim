# Geology plausibility benchmark

`worldsim_geology_benchmark` measures the tectonic generator and the initialized stateful geology surface against broad geological observables without mutating simulation history.

It is a **plausibility benchmark**, not a claim that WorldSim implements full physical plate evolution. WorldSim now carries persistent crust thickness/density, lithosphere age, sediment mass and regolith state, and evolves those fields through a deliberately reduced long-term process model. Scientific reference mismatches are reported as warnings in the generated JSON/stdout; they do not fail CTest. Kernel invariants and executable failures remain normal test failures.

## Why these observables

The benchmark follows established global-geology observables rather than comparing screenshots by eye.

### Plate-area spectrum and boundary kinematics

Bird's PB2002 present-day plate model contains 52 plates and reports a cumulative plate-number/area power-law regime for plate areas from 0.002 to 1 steradian. It also classifies digitized boundaries by tectonic regime and relative motion. PB2002 treats a step as strike-slip when relative velocity lies within +/-20 degrees of the boundary azimuth; WorldSim's coarse transform diagnostic uses the same angular rule.

Reference:

- Peter Bird (2003), *An updated digital model of plate boundaries*, Geochemistry, Geophysics, Geosystems 4(3), 1027. https://doi.org/10.1029/2001GC000252

WorldSim does not attempt to reproduce 52 Earth plates. The benchmark instead records whether a fixed 16-plate world has any meaningful hierarchy of plate areas and center spacings at all:

- plate-area coefficient of variation;
- largest/smallest plate-area ratio and log span;
- normalized area entropy;
- nearest-neighbor center-spacing variation;
- resolved plate adjacency degree;
- coarse convergent/divergent/transform boundary-length fractions.

PB2002 Table 3 totals imply coarse present-day length fractions of about 35.2% convergent (CCB + OCB + SUB), 36.3% divergent (CRB + OSR), and 28.3% transform (CTF + OTF). WorldSim emits a reference warning only when an aggregate class differs by more than 15 percentage points; this is deliberately much looser than an Earth-calibration criterion.

The current warning `plate_geometry_too_regular` is deliberately conservative: it fires when the mean largest/smallest area ratio is below 2 or center-spacing CV is below 0.10. Those numbers are **diagnostic floors**, not Earth calibration targets. PB2002's published area spectrum spans orders of magnitude; a nearly equal-area 16-plate partition cannot express that type of hierarchy.

Plate-layout diversity is also bounded away from a different numerical pathology: near-coincident Voronoi seeds. The generator starts from a 16-point Fibonacci scaffold, applies the deterministic large jitter needed for area diversity, and backs that jitter off when necessary to retain at least 20 degrees between accepted seed centers while keeping later scaffold anchors available. The 20-degree separation is an engineering guard for this low-resolution analytical model, not a claimed Earth plate-spacing law.

### Crust topology

High-affinity crust is sampled on a same-level cube-sphere cover and thresholded at `continental_affinity >= 0.5`. The benchmark reports:

- high-affinity area fraction;
- connected component count;
- fraction of high-affinity area in the largest component;
- high/low-affinity transition-edge fraction;
- macro-relief contrast between high- and low-affinity crust.

These remain generator diagnostics. `continental_affinity` is the smooth tectonic control used to initialize persistent crust thickness and density; it is not itself a rock-composition measurement, so the benchmark does not equate its threshold directly with measured continental crust.

For context, modern-Earth literature describes a strong continental/oceanic lithosphere distinction and bimodal hypsometry:

- Cawood et al. (2022), *Secular Evolution of Continents and the Earth System*. https://doi.org/10.1029/2022RG000789
- Forte et al. (2022), *Earth's Isostatic and Dynamic Topography—A Critical Perspective*. https://doi.org/10.1029/2021GC009740

### Stateful geological evolution

The authoritative simulation surface is no longer regenerated directly from `TerrainGenerator(seed)` after build. Geography stores persistent geological state in the normal adaptive field store:

- `geology.crust_thickness_m` and `geology.crust_density_kg_m3`;
- evolving `geology.continental_fraction`, so long-lived rifting can complete breakup instead of thinning an immutable continental label forever;
- `geology.lithosphere_age_ma`;
- extensive `geology.sediment_mass_kg`, which therefore conserves mass under LOD split/merge;
- `geology.regolith_thickness_m`;
- diagnostic `geology.erosion_rate_m_yr`, `geology.drainage_area_m2` and `geology.drainage_discharge_m3_day`;
- derived `geology.trench_forcing`, `geology.volcanic_arc_forcing`, `geology.collision_forcing` and `geology.rift_forcing` for process inspection.

The daily geology system advances these fields using simulation elapsed time. Continental convergence stores shortening as crustal thickening; continental divergence thins crust and progressively reduces the persistent continental fraction; once breakup crosses into an oceanic regime, divergence renews young thin crust instead of continuing unlimited continental thinning. Convergent plate pairs receive deterministic, pair-stable subduction polarity: the subducting side forms a compact trench and loses crust, while the overriding side receives a volcanic-arc forcing offset inland from the boundary and modest magmatic crustal addition. High-continental-fraction convergence switches to broad collision forcing instead of creating an artificial subduction trench. Surface elevation is then derived from crustal buoyancy/isostasy, oceanic thermal age, sediment load, boundary response and bounded meso-scale roughness. A short-range neighbor coupling approximates lithospheric flexure without claiming a full elastic/viscoelastic plate solver.

Oceanic age-depth behavior follows the broad empirical form documented by Parsons and Sclater: young ocean floor deepens approximately with the square root of age, while older lithosphere approaches a plate-model asymptote.

- Parsons, B. & Sclater, J. G. (1977), *An analysis of the variation of ocean floor bathymetry and heat flow with age*. https://doi.org/10.1029/JB082i005p00803

Erosion is slope/runoff driven using a bounded stream-power-like law. The geology pass first builds a strictly downhill routing graph from the current surface, accumulates contributing land area and runoff in descending-elevation order, and uses that accumulated discharge rather than only local rainfall/runoff to drive incision. Eroded sediment and bedrock are converted to transported mass and deposited along the same downstream routing weights; transport updates are accumulated before application so iteration order cannot create or destroy sediment mass. Deposited sediment increases geometric surface thickness while its load produces partial isostatic subsidence, so basin infill has the correct net sign.

- Whipple, K. X. & Tucker, G. E. (1999), *Dynamics of the stream-power river incision model*. https://doi.org/10.1029/1999JB900120

Cross-cell transport does not treat a same-level cube-sphere neighbor as if it were necessarily an active simulation leaf. At a coarse/fine interface WorldSim restricts the active descendants by area to compare neighbor elevation and distributes transported extensive sediment by area. This follows the conservative coarse/fine synchronization principle used by established AMR schemes rather than selecting an arbitrary refined child.

- Berger, M. J. & Colella, P. (1989), *Local adaptive mesh refinement for shock hydrodynamics*. https://doi.org/10.1016/0021-9991(89)90035-1
- AMReX documentation, *Using FluxRegisters*: coarse/fine conservation compares area- and time-weighted fluxes and corrects mismatches at refinement interfaces. https://amrex-codes.github.io/amrex/docs_html/AmrCore.html

The model is intentionally a reduced geological closure, not a 3-D mantle or thermo-mechanical lithosphere solver. Elastic-plate flexure remains the physical interpretation of the bounded spatial load response rather than a claim of detailed rheology.

### Hypsometry

NOAA/NCEI ETOPO provides a global relief model used to derive Earth's hypsographic curve:

- https://www.ngdc.noaa.gov/mgg/global/relief/

USGS summarizes modern Earth as about 71% water-covered surface:

- https://www.usgs.gov/water-science-school/science/how-much-water-there-earth

The benchmark therefore records, using equal-area spherical probes:

- above-sea land fraction;
- elevation mean, standard deviation and percentiles;
- deep-ocean and high-mountain fractions;
- a coarse ocean-mode and land-mode estimate from 250 m histogram bins;
- mode separation;
- correlation between authoritative terrain and tectonic macro relief.

The broad 15..45% land warning (checked both on the aggregate mean and for per-seed outliers) and 2.5 km mode-separation warning are sanity ranges only. The benchmark records coarse ocean and land histogram modes for inspection, but it does not gate the absolute land-mode elevation. ETOPO1's published hypsographic summary places the continental grouping several hundred meters above sea level and reports an average land height near 800 m, so a universal +/-500 m land-mode threshold is not supported by that reference. These warnings are not intended to force every generated rocky planet to reproduce modern Earth.

### Spatial coupling

A tectonic-looking map is insufficient if the fields are causally unrelated. The benchmark also measures:

- uplift/divergence active coverage;
- crust-matched macro-elevation excess in uplift-active samples;
- oceanic divergence elevation response;
- continental divergence elevation response.

The uplift comparison bins samples by crust affinity before comparing active and inactive samples so a crust-buoyancy difference is not mistaken for orogenic uplift.

## Sampling

Two sampling schemes are used for different contracts:

1. **cube-sphere uniform cover** for plate adjacency and crust connectivity. The benchmark uses `CubeSphereTopology::neighbors4()` only on a same-level uniform cover, which is within that API's documented contract.
2. **equal-area Fibonacci probes** for global distributions and correlations. This avoids equirectangular latitude weighting.

The default CI baseline is:

```bash
./out/dev/worldsim_geology_benchmark \
  --seed-count 64 \
  --samples 4096 \
  --cover-level 5 \
  --output out/geology-benchmark
```

Developer shortcut:

```bash
make geology
```

The CTest smoke uses `--quick` (4 seeds, 1024 equal-area probes, level-4 cover) to validate executable integration without turning scientific reference warnings into build failures.

## Output

`geology_benchmark.json` contains:

- method/sampling metadata;
- explicit Earth reference context;
- aggregate 64-seed metrics;
- warning/info findings;
- all per-seed measurements.

CI publishes this directory as the `worldsim-geology-benchmark` artifact.

## What this benchmark cannot validate yet

The v1 model now has enough state to make process-level assertions about crustal thickness/density, continental breakup, lithosphere age, thermal subsidence, asymmetric trench/volcanic-arc geometry, reduced isostasy, erosion and sediment mass transport. It still does **not** validate:

- plate velocities in physical angular-rate units or time-evolving plate geometry;
- slab geometry, mantle convection or time-dependent trench migration beyond the reduced pair-stable subduction polarity;
- a solved elastic/viscoelastic lithosphere with spatially varying effective elastic thickness;
- rock-type/mineral phase evolution, metamorphism or explicit crust/mantle chemistry;
- multi-layer sediment stratigraphy, compaction and marine transport;
- glacial, aeolian, coastal and groundwater geomorphology;
- supercontinent-cycle plate reconstruction.

Those are fidelity extensions rather than missing closure in the current terrain-causality loop. New benchmark gates should be added only when the model gains a corresponding explicit physical contract.

# Geology plausibility benchmark

`worldsim_geology_benchmark` measures the current static tectonic/terrain generator against broad geological observables without changing authoritative generation behavior.

It is a **plausibility benchmark**, not a claim that WorldSim implements physical plate evolution. Scientific reference mismatches are reported as warnings in the generated JSON/stdout; they do not fail CTest. Kernel invariants and executable failures remain normal test failures.

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

### Crust topology

High-affinity crust is sampled on a same-level cube-sphere cover and thresholded at `continental_affinity >= 0.5`. The benchmark reports:

- high-affinity area fraction;
- connected component count;
- fraction of high-affinity area in the largest component;
- high/low-affinity transition-edge fraction;
- macro-relief contrast between high- and low-affinity crust.

These are generator diagnostics. `continental_affinity` is not yet a physical crust-thickness/composition model, so the benchmark does not equate its threshold directly with measured continental crust.

For context, modern-Earth literature describes a strong continental/oceanic lithosphere distinction and bimodal hypsometry:

- Cawood et al. (2022), *Secular Evolution of Continents and the Earth System*. https://doi.org/10.1029/2022RG000789
- Forte et al. (2022), *Earth's Isostatic and Dynamic Topography—A Critical Perspective*. https://doi.org/10.1029/2021GC009740

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

The broad 15..45% land warning and 2.5 km mode-separation warning are sanity ranges only. A separate modern-Earth reference warning fires when the coarse land-elevation mode is more than 500 m from sea level; modern-Earth literature places the continental/land hypsometric mode at or near sea level. These warnings are not intended to force every generated rocky planet to reproduce modern Earth.

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

The current analytical model does not contain enough state to validate:

- plate velocities in physical angular-rate units;
- time evolution of plate geometry;
- oceanic crust production, age and thermal subsidence;
- subduction polarity, slab consumption or trench geometry;
- crustal thickness, density, isostatic balance or compositional evolution;
- transform-fault localization;
- sedimentation, erosion or orogenic age;
- supercontinent cycles.

Those require additional model state before a scientific validation claim is meaningful. The benchmark should gain new observables only when the generator gains the corresponding physical contract.

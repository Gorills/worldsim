# Long-run world stability

WorldSim has a reproducible long-run harness for separating initialization
transients from fire, fauna and coupled-system failures:

```bash
cmake --preset dev
cmake --build --preset dev --target worldsim_long_run
./out/dev/worldsim_long_run \
  --years 100 --level 2 --seed 42 --mode coupled \
  --assert-stable --output out/long-run/seed42.csv
```

`make longrun` runs the same 100-year level-2 coupled case. The executable also
accepts `no-fire`, `no-fauna` and `no-fire-no-fauna` modes. All four modes use
the same seed, climate, hydrology, geology, soil and vegetation code; only the
named disturbance/consumer systems are omitted. Registered fields and stores
stay identical so CSV schemas remain directly comparable. In the current
`no-fauna` mode the fauna system is omitted but initialized cohorts remain as
frozen baseline state; the mode isolates fauna dynamics/consumption rather than
constructing an animal-free initial world.

## Stability matrix

A single seed/resolution can catch catastrophic drift without showing whether
the same coupled model behaves consistently across different generated worlds
or simulation resolutions. The matrix driver runs the existing
`worldsim_long_run` executable independently for every requested combination;
it does not introduce a second simulation path:

```bash
make stability
```

The default matrix is 100 coupled years for seeds `0,42,999` at uniform
levels `2,3`. Level 2 is the minimum quantitative baseline: the initial
level-1/2 probe showed up to 65.7% represented-land-area aliasing, so level 1
remains useful for bounded engineering smoke but not convergence claims.
Override the matrix without changing the harness, for example:

```bash
STABILITY_YEARS=20 STABILITY_SEEDS=42 STABILITY_LEVELS=2,3 make stability
STABILITY_YEARS=30 STABILITY_MODES=coupled,no-fire,no-fauna make stability
```

The output directory contains each original yearly case CSV and log plus:

- `matrix.csv`: the final row of every successful case;
- `resolution.csv`: adjacent-level absolute and symmetric relative deltas for
  represented land area, vegetation density/retention, PFT shares, litter,
  soil carbon, total and land-normalized NPP/fauna carbon, fauna nitrogen,
  fire, fertility, mineral-N density, planetary-N residual, temperature and
  precipitation;
- `summary.json`: exact matrix inputs, failures and maximum observed
  adjacent-level deltas.

The existing `--assert-stable` envelope remains the only acceptance gate.
Cross-resolution deltas are intentionally diagnostic until a multi-seed
baseline exists; imposing a percentage threshold before measuring the current
model would turn an arbitrary number into a false convergence claim. Land area
is reported separately because coarse cell-center geography can change the
represented terrestrial area; total NPP/fauna carbon and their per-land-area
densities are both retained so those effects are not conflated.

Regular PR CI runs a short two-year coupled smoke for seeds `0,42,999` at
levels `2,3` and uploads the complete matrix artifact. That verifies
orchestration, multi-seed execution and both resolutions, but it is **not**
100-year release evidence. Release/model-calibration work should run the
longer matrix explicitly and include a selected higher-resolution comparison.

This experiment-matrix approach follows the same validation direction as
FireMIP2: standardized scenario definitions and consistent outputs are used to
separate model behavior from one chosen realization. WorldSim's matrix is much
smaller and is not an Earth-system intercomparison:

- Hantson et al. (2026), *The Fire Modeling Intercomparison Project phase 2
  (FireMIP2)*: https://gmd.copernicus.org/articles/19/3989/2026/

## What the harness reports

Each row is one simulation year. It includes:

- global and connected-land-component vegetation retention;
- functional-type shares, litter, soil carbon, mineral-N density, derived fertility and NPP;
- annual and cumulative burned area, emissions, char and active-fire area;
- herbivore/carnivore counts plus derived fauna carbon and nitrogen;
- cumulative fauna respiration;
- area-weighted land climate, atmospheric CO2, closed planet-carbon residual,
  terrestrial-N retention, atmospheric/ocean N reservoirs, cumulative
  fixation/deposition, closed planet-N residual, water/energy residuals and
  invalid values;
- the fraction of initially vegetated land whose biomass fell below 10% of its
  own initial density.

`fire_burned_area_m2` is a cumulative activity ledger. Reburning counts again,
so it may exceed the land area and cannot answer "what unique fraction of a
continent has ever burned?" Exact unique footprint would require persistent
sub-cell burn geometry or an explicit disturbance-age state; the current
coarse process has neither.

## Regression envelope

`--assert-stable` fails when any of these deliberately broad engineering
conditions is violated at the end of the requested run:

- any registered field is non-finite or outside its descriptor bounds;
- the closed planet-carbon inventory drifts by more than `1e-10` relative to
  its initialized stock;
- the closed planetary nitrogen inventory drifts by more than `1e-10`
  relative to its initialized stock;
- global vegetation is below 50% or above 400% of its initialized stock;
- more than 25% of initially vegetated land falls below 10% of its own initial
  density;
- any connected land component retains less than 10% of its initial biomass;
- fauna carbon falls below 1% or exceeds 32 times its initialized stock.

These thresholds catch the failures found in the September 2026 audit. They
are not an Earth calibration, a proof of equilibrium or a promise that every
seed and resolution is converged. The CTest smoke exercises ten coupled years
at level 2 plus a one-year level-2 no-fauna acceptance regression. PR CI additionally
runs the short matrix described above. Release evidence should still cover the
full multi-seed matrix, at least one 100-year run, and a higher-resolution
comparison.

## Resolution-convergence correction — 2026-09-09

The first level-2/3 matrix isolated a real climate discretization error rather
than only geography aliasing. On seed 0, represented land area differed by just
0.5%, yet after two coupled years vegetation density differed by 45.2% and NPP
per land area by 49.6%. A separate flat 50/50 land/ocean climate fixture removed
geology, hydrology, ecology, fire and fauna from the comparison and measured a
3.521772 K mean absolute L2-versus-aggregated-L3 temperature difference.

The cause was horizontal heat exchange: every neighboring climate-node pair
used the same fixed relaxation fraction regardless of center distance or shared
interface. Refining the climate reference grid therefore changed effective
lateral heat diffusivity with cell size. Heat exchange now uses a conservative
finite-volume conductance based on a fixed effective diffusivity, symmetric
areal heat capacity, physical shared-interface length and center distance. The
pair transfer remains antisymmetric and analytically bounded by pair
equilibrium.

The same flat fixture now measures 0.261492 K L2/L3 mean absolute temperature
difference. The two-year three-seed coupled matrix changed as follows (maximum
adjacent-level relative delta):

- vegetation density: 45.2% -> 10.3%;
- vegetation retention ratio: 41.2% -> 3.7%;
- NPP per represented land area: 49.6% -> 11.7%;
- fauna carbon ratio: 8.8% -> 1.7%;
- mean land precipitation: 14.9% -> 8.2%.

The climate correction left a separate geography defect: on the current
carbon/nitrogen/fauna main baseline the same two-year, seeds 0/42/999 L2/L3
matrix still measured a maximum 18.86% represented-land-area delta. Coarse
`geography.land_fraction` was being reconstructed from one cell-center
elevation even though downstream domains use it as physical terrestrial area.

The coastal-geography correction replaces that center classification below
level 4 with an area-integrated fixed level-4 reference support and initializes
coarse geology as the conservative restriction of the same support. A pure
focus-LOD transition preserves the already represented land area rather than
reclassifying it. On the same current-main two-year matrix, maximum L2/L3
represented-land-area delta falls from 18.86% to **1.51%**.

This removes the measured area aliasing but does not claim complete coupled
convergence. Compared with the same current-main baseline, several maximum
relative deltas improve (mean total NPP 29.89% -> 26.01%, fauna carbon density
10.81% -> 7.65%, mean fertility 30.70% -> 24.45%, mineral-N density 45.16% ->
25.37%, mean land temperature 1.06% -> 0.88%), while others become larger
(vegetation density 13.92% -> 23.34%, NPP density 21.37% -> 26.39%, fauna
carbon ratio 2.55% -> 9.66%, mean land precipitation 7.99% -> 21.33%). The
remaining differences are therefore process/spatial diagnostics, not evidence
that closing land area alone makes every coupled field converge. Orographic
forcing and other coarse derived-surface operators remain candidates for a
separate resolution-quality slice.

Fire is thresholded and path-dependent: a zero-versus-small nonzero burn can
still report a 100% relative delta, so its absolute burned fraction and
longer-run statistics must be interpreted separately.

These two-year results are regression evidence for the corrected transport and
coastal-area operators, not proof of long-run convergence or calibration. The
next release evidence remains the explicit 100-year multi-seed level-2/3
matrix.

### Orographic reference correction — 2026-09-09

The remaining precipitation-resolution diagnostic was traced to climate uplift
forcing reading the representative cell-center elevation. Geography now
provides a fixed-support, area-mean orographic reference below level 4 while
retaining the existing evolving center elevation for geology and thermal lapse
semantics. A regression-only commit demonstrated that the former L2/L3
orographic references were not area-consistent.

On the same two-year coupled seeds `0,42,999`, levels `2,3` matrix used for
the coastal baseline, the final bounded correction changes maximum
adjacent-level relative deltas as follows:

- mean land precipitation: 21.33% -> **19.25%**;
- mean total NPP: 26.01% -> **20.77%**;
- NPP density: 26.39% -> **21.19%**;
- mean fertility: 24.45% -> **16.62%**;
- mineral-N density: 25.37% -> **23.78%**;
- fauna carbon density: 7.65% -> **7.41%**;
- litter density: 4.12% -> **3.53%**;
- soil-carbon density: 4.38% -> **4.28%**.

Not every diagnostic improves: vegetation density moves from 23.34% to 23.55%,
vegetation retention from 16.12% to 16.35%, and mean land temperature from
0.877% to 0.898%. Those changes are small relative to the primary improvement
and are retained rather than changing unrelated thermal or ecology closures.

Two broader variants were explicitly rejected during the slice. Reusing the
area-mean orographic reference for thermal initialization improved temperature
agreement but worsened NPP density to 29.78% and mineral-N density to 43.41%.
Changing the capped linear orographic rainout law to a compositional
exponential closure worsened precipitation to 25.75% and NPP density to
34.77%. The merged design therefore changes only the verified
center-sampled-orography failure mode.

The standard PR CI still runs the two-year matrix, and CTest includes its
existing ten-year level-2 coupled smoke. A fresh 100-year multi-seed L2/L3
matrix was **not** run for this correction; it remains release evidence rather
than a claim made by these PR diagnostics.

### Stochastic weather support correction — 2026-09-09

The next resolution invariant was stochastic rather than geometric. Weather
anomalies were generated from `seed + tick + climate-cell id`, so level 2 and
level 3 received unrelated random forcing over the same physical hierarchy
regions. A regression-only head failed exactly the new area-aggregate weather
test while all other CTest entries and Godot integration passed.

The corrected operator keeps the existing level-4 random stream authoritative
and area-averages those samples for levels 1-3. The fixed support is cached when
the ClimateStore graph is built. Default level-4 simulations therefore retain
the former random sequence, while coarse quantitative baselines no longer
invent a separate weather realization solely because their climate reference
level changed.

On the same two-year coupled seeds `0,42,999`, levels `2,3` matrix, the
change is deliberately modest:

- mean land precipitation: 19.2486% -> **19.2057%**;
- mean total NPP: 20.7725% -> **20.4789%**;
- NPP density: 21.1858% -> **20.8937%**;
- mineral-N density: 23.7750% -> **23.6755%**;
- mean land temperature: 0.8984% -> **0.8956%**.

Vegetation-density and litter-density maxima move by only +0.0016 and +0.0022
percentage points respectively. The remaining roughly 19% precipitation delta
therefore cannot be attributed primarily to independent stochastic weather.
Smooth center-sampled climate operators such as prescribed wind/insolation and
the nonlinear moisture/condensation discretization remain separate candidates;
they should be isolated with controlled fixtures rather than tuned from the
coupled matrix.

The cached fixed-support implementation passed 15/15 CTest, the complete
six-case stability smoke, visual/geology diagnostics and Godot 4.7 integration.
A fresh 100-year multi-seed matrix is still not claimed by this slice.

### Natural-fire stochastic support correction — 2026-09-09

The short coupled matrix showed that wildfire stochasticity itself was strongly
resolution-dependent. The ignition hazard already scaled with effective land
area, but ignition and ignition-size draws were keyed by the current active
cell. Under otherwise identical dry/fueled fixtures, seed 0 first ignited on
day 9 at level 2 and day 8 at level 3 with different burned-area magnitudes.

Natural ignition now runs on fixed level-4 reference regions. Active cells
contribute hazard to those regions; each reference region performs one
stateless ignition/size draw; and mixed/refined covers assign a successful
ignition to one active descendant in proportion to hazard contribution. A
uniform level-4 world keeps the former random stream and cell key, while coarse
and refined worlds no longer create duplicate or unrelated ignition processes.

On the same two-year coupled seeds `0,42,999`, levels `2,3` smoke:

- maximum absolute annual burned-fraction gap: about **21.64 pp -> 10.30 pp**;
- seed 0 absolute burn gap: about **2.08 pp -> 0.0056 pp**;
- seed 999: both levels are zero in this short window;
- maximum NPP-density delta: **20.89% -> 16.78%**;
- maximum fauna-carbon-density delta remains about **7.41%**;
- precipitation and temperature deltas are effectively unchanged.

The relative fire delta is intentionally not used as the primary acceptance
metric here because zero-versus-near-zero burns can report values near 100%
while the absolute affected area is negligible. Individual ecology/N diagnostics
also move in both directions by seed because synchronizing fire changes the
disturbance path rather than applying a smooth perturbation.

This correction changes stochastic continuation from identical snapshots, so
the combined snapshot epoch advances from 36 to 37 despite unchanged binary
field/store layouts. A fresh 100-year multi-seed matrix is still not claimed by
this slice.

### Spatial-ecology face-geometry correction — 2026-09-10

After the climate, hydrology, coastline and stochastic-ignition corrections,
three remaining cross-cell ecology processes still interpreted an adaptive
neighboring hierarchy region as a cell hop rather than as a physical
interface. A controlled regression represented the same pair of level-2
physical regions directly at level 2 and by their level-3 children. Before any
production change, the one-step aggregates diverged by nearly the same factor:

- grass recruitment: L2 110,986,269.77 kg versus L3 59,902,690.55 kg,
  **46.03%**;
- herbivore arrival: L2 17.898112 versus L3 9.660160 individuals,
  **46.03%**;
- source-driven active fire area: L2 44,375,746,173.55 m2 versus
  L3 24,002,740,987.74 m2, **45.91%**.

The common failure was spatial semantics, not three independent ecological
parameters. `active_neighbors4()` intentionally resolves a hierarchy region
with area-allocation weights; those weights do not describe the length of the
shared face. Flora establishment, fire spread and fauna movement now use a
separate `active_face_neighbors4()` contract that returns only physically
touching active leaves plus their spherical interface lengths.

Each process retains its former uniform level-4 calibration by converting
interface length through a fixed level-4 reference face depth. Coarser or
refined covers integrate that same physical support instead of changing the
process merely because cell size changed. The retained controlled regressions
require the level-2/level-3 aggregate discrepancy to remain below 5%. Mixed-LOD
tests additionally cover cube-face seams, unbalanced refinement deeper than one
level and cache invalidation after cover changes.

The new face-neighborhood geometry is derived state, not snapshot state. It is
cached only for the current active cover and discarded on refine/coarsen,
matching the separation between adaptive topology and derived mesh-neighborhood
data used by AMR systems such as p4est:
https://p4est.github.io/p4est-howto.pdf. Fauna keeps the existing
habitat-selection model; the movement-geometry factor only separates movement
capacity from destination quality, consistent with the movement/resource
selection separation described by Avgar et al. (2016):
https://doi.org/10.1111/2041-210X.12528.

Because identical saved state now resumes under different deterministic spatial
process semantics, the combined snapshot compatibility epoch advances from 38
to 39. A fresh 100-year multi-seed matrix is not claimed by this bounded slice.

## Failures found and model changes

The original initialization assigned up to a 4 kgC/m2 biomass scale before the
coupled climate/hydrology state had evolved. A 100-year level-1 seed-42 run with
both fire and fauna disabled converged near 0.544 kgC/m2 and retained only
25.6% of the old initial stock. This was an initialization drawdown rather than
an extinction attractor. Initialization now uses a 1 kgC/m2 scale, which starts
the same case near its observed undisturbed attractor without adding an
expensive hidden simulation to every constructor.

Fauna previously multiplied cohort counts directly from a food-satisfaction
ratio. New animal bodies did not debit any carbon reservoir, ordinary death
did not return biomass, and a 100-year run could grow herbivores by roughly
26,000 times. Carbon accounting fixed that failure; fauna nitrogen
stoichiometry now applies the same material-ownership rule to N. Fauna uses the
following explicit reduced closure:

- cohort carbon is `count * (body mass + reserve) * 0.15`;
- herbivore and carnivore growth can use only carbon assimilated from removed
  forage/prey;
- unassimilated food and mortality enter litter;
- maintenance enters `ecology.fauna_respired_carbon_kg`;
- herbivore forage removal is capped at 2.5% of standing preference-weighted
  forage per simulated day and scales with the elapsed fauna-step interval;
- standing herbivore carbon is density-regulated around `1e-4` of
  preference-weighted forage carbon;
- carnivore carbon is density-regulated around 8% of herbivore carbon;
- cohort nitrogen is derived at fixed body C:N=4 and participates in the closed
  planetary N inventory;
- final animal biomass cannot retain more N than prior body N plus consumed
  plant/prey N; excess assimilated carbon is respired;
- dietary N not retained in body biomass, plus N released by net mortality, is
  recycled 65% to litter and 35% to mineral N.

New worlds initialize the herbivore guild at one tenth of its single-guild
forage ceiling, then initialize carnivores from the same 8% trophic ratio. This
keeps predators present while avoiding the artificial century-scale drawdown
caused by starting both guilds at the no-predator herbivore ceiling.

The legacy integrated ecology test still closes vegetation + litter + soil +
fauna + fire stocks and cumulative boundary ledgers against reported NPP. The
planetary climate regression additionally closes atmosphere + ocean + live
vegetation + litter + soil + fauna + pyrogenic carbon as stocks, explicitly
excluding cumulative respiration/fire ledgers from the inventory. A separate
overcrowding regression verifies that abundant food no longer implies
unbounded population growth.

The wet-mass carbon fraction, fixed fauna C:N, assimilation efficiencies and
biomass-pyramid ratios are reduced game-model parameters. They are intentionally labeled as
not Earth-calibrated and should later be replaced or calibrated per functional
guild when species physiology becomes an actual domain.

## Modeling precedent

The Fire Model Intercomparison Project protocol separates `nofire` experiments
and uses long vegetation spin-up/equilibrium phases before coupled fire
analysis. That is the precedent for the harness modes and for treating the old
startup collapse separately from fire response:

- Hantson et al. (2026), *The Fire Model Intercomparison Project phase 2
  (FireMIP2)*: https://gmd.copernicus.org/articles/19/3989/2026/

The Madingley general ecosystem model derives growth from assimilated food
after metabolic costs and evaluates persistence over millennial integrations.
WorldSim remains vastly simpler, but follows the same essential accounting
direction: demographic gains must be paid by ingested material rather than by
an unconstrained count multiplier.

- Harfoot et al. (2014), *Emergent Global Patterns of Ecosystem Structure and
  Function from a Mechanistic General Ecosystem Model*:
  https://journals.plos.org/plosbiology/article?id=10.1371/journal.pbio.1001841

## Current limits and next calibration gate

Long runs are now useful as regression evidence. They still do not establish
real-world accuracy. Fire coefficients should be calibrated only after the
no-fire baseline remains stable across the target seed/resolution matrix.
After that gate, compare annual burned fraction, NPP, biome/PFT composition,
soil-carbon turnover, atmospheric CO2 and trophic biomass against explicit game
targets or observational datasets. Carbon cycle v1 is an engineering closure,
not an Earth calibration: geologic/fossil carbon, carbonate chemistry, explicit
ocean circulation, phosphorus and other nutrient budgets, resolved reactive-N
chemistry/speciation, aquatic nutrient transport/food webs and unique burn
footprints remain out of scope. Planetary nitrogen v2 adds a conservative reduced N2/reactive/ocean box with
fixation/deposition/denitrification. Fauna stoichiometry v1 additionally makes
animal biomass an N reservoir with homeostatic trophic transfers. Reservoir
sizes, exchange coefficients and fauna C:N remain engineering parameters
rather than calibrated terrestrial or marine biogeochemistry.

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
  soil carbon, total and land-normalized NPP/fauna carbon, fire, fertility,
  mineral-N density, tracked-N residual, temperature and precipitation;
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
- herbivore/carnivore counts plus derived fauna carbon;
- cumulative fauna respiration;
- area-weighted land climate, atmospheric CO2, closed planet-carbon residual,
  tracked-nitrogen residual, water/energy residuals and invalid values;
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
- the tracked terrestrial nitrogen inventory plus explicit leaching/fire
  boundary ledgers drifts by more than `1e-10` relative to its initialized
  amount;
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

Represented land area still differs by up to 18.9% between levels 2 and 3 for
the sampled seeds because coarse geography remains cell-center sampled. Total
stocks/flows therefore retain some structural resolution dependence even when
their land-normalized process rates are closer. Fire is thresholded and
path-dependent: a zero-versus-small nonzero burn can still report a 100%
relative delta, so its absolute burned fraction and longer-run statistics must
be interpreted separately.

These two-year results are regression evidence for the corrected transport
operator, not proof of long-run convergence or calibration. The next release
evidence remains the explicit 100-year multi-seed level-2/3 matrix.

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
26,000 times. Fauna now uses the following explicit reduced closure:

- cohort carbon is `count * (body mass + reserve) * 0.15`;
- herbivore and carnivore growth can use only carbon assimilated from removed
  forage/prey;
- unassimilated food and mortality enter litter;
- maintenance enters `ecology.fauna_respired_carbon_kg`;
- herbivore forage removal is capped at 2.5% of standing preference-weighted
  forage per simulated day and scales with the elapsed fauna-step interval;
- standing herbivore carbon is density-regulated around `1e-4` of
  preference-weighted forage carbon;
- carnivore carbon is density-regulated around 8% of herbivore carbon.

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

The wet-mass carbon fraction, assimilation efficiencies and biomass-pyramid
ratios are reduced game-model parameters. They are intentionally labeled as
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
ocean circulation, phosphorus and other nutrient budgets, atmospheric
nitrogen chemistry/fixation/deposition, aquatic nutrient transport/food webs
and unique burn footprints remain out of scope. Nitrogen cycle v1 is likewise
an engineering conservation/limitation closure, not a calibrated terrestrial
biogeochemistry model.

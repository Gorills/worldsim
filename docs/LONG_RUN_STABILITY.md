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
levels `1,2`. Override it without changing the harness, for example:

```bash
STABILITY_YEARS=20 STABILITY_SEEDS=42 STABILITY_LEVELS=2,3 make stability
STABILITY_YEARS=30 STABILITY_MODES=coupled,no-fire,no-fauna make stability
```

The output directory contains each original yearly case CSV and log plus:

- `matrix.csv`: the final row of every successful case;
- `resolution.csv`: adjacent-level absolute and symmetric relative deltas for
  vegetation density/retention, PFT shares, litter, soil carbon, NPP, fire,
  fauna, fertility, temperature and precipitation;
- `summary.json`: exact matrix inputs, failures and maximum observed
  adjacent-level deltas.

The existing `--assert-stable` envelope remains the only acceptance gate.
Cross-resolution deltas are intentionally diagnostic until a multi-seed
baseline exists; imposing a percentage threshold before measuring the current
model would turn an arbitrary number into a false convergence claim.

Regular PR CI runs a short two-year coupled smoke for seeds `0,42,999` at
levels `1,2` and uploads the complete matrix artifact. That verifies
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
- functional-type shares, litter, soil carbon, fertility and NPP;
- annual and cumulative burned area, emissions, char and active-fire area;
- herbivore/carnivore counts plus derived fauna carbon;
- cumulative fauna respiration;
- area-weighted land climate, water/energy residuals and invalid values;
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
- global vegetation is below 50% or above 400% of its initialized stock;
- more than 25% of initially vegetated land falls below 10% of its own initial
  density;
- any connected land component retains less than 10% of its initial biomass;
- fauna carbon falls below 1% or exceeds 32 times its initialized stock.

These thresholds catch the failures found in the September 2026 audit. They
are not an Earth calibration, a proof of equilibrium or a promise that every
seed and resolution is converged. The CTest smoke exercises ten coupled years
at level 1 plus a one-year no-fauna acceptance regression. PR CI additionally
runs the short matrix described above. Release evidence should still cover the
full multi-seed matrix, at least one 100-year run, and a higher-resolution
comparison.

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

The integrated daily test closes vegetation + litter + soil + fauna + fire
stocks and cumulative boundary ledgers against reported NPP. A separate
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
soil-carbon turnover and trophic biomass against explicit game targets or
observational datasets. Unique burn footprint, fire-return interval, nutrient
budgets, aquatic food webs and atmospheric fauna-carbon exchange remain out of
scope.

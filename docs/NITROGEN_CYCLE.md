# Nitrogen cycle v2

Nitrogen v1 introduced explicit terrestrial N stocks and finite-N plant
limitation. Planetary nitrogen v2 closes the remaining one-way boundary by
adding non-spatial atmospheric and ocean reservoirs plus reduced return
processes. The goal is conservative long-run causality, not Earth calibration.

## Authoritative stocks

Terrestrial authoritative stocks remain extensive fields:

- `ecology.litter_nitrogen_kg`;
- `ecology.soil_fast_nitrogen_kg`;
- `ecology.soil_slow_nitrogen_kg`;
- `ecology.mineral_nitrogen_kg`;
- `ecology.grass_nitrogen_kg`;
- `ecology.shrub_nitrogen_kg`;
- `ecology.tree_nitrogen_kg`.

`ecology.vegetation_nitrogen_kg` is the derived sum of PFT N and is not an
additional stock. `ecology.soil_fertility` is a bounded diagnostic of
mineral-N density.

`NitrogenStore` owns three global reservoirs independently of adaptive LOD:

- reduced atmospheric N2 donor;
- reactive atmospheric nitrogen;
- ocean dissolved nitrogen.

The reduced initialization uses 0.50 kgN/m2 of total planetary surface for the
N2 donor and 0.05 kgN/m2 of initialized ocean area for dissolved ocean N.
These deliberately compressed reservoirs keep the model numerically useful;
they are not claimed Earth inventories.

## Boundary rates versus audit ledgers

Nitrogen v1 cumulative fields remain immutable audit histories:

- `ecology.nitrogen_leached_kg`;
- `ecology.fire_emitted_nitrogen_kg`.

Planetary transfers never infer a daily flux by differencing those histories.
Soil and fire now expose current extensive rates:

- `ecology.nitrogen_leaching_kg_day`;
- `ecology.fire_nitrogen_emission_kg_day`.

At the final daily `ecology.nitrogen_cycle` pass, realized leaching enters the
ocean reservoir and realized fire emission enters the reactive atmosphere.
The cumulative ledgers are excluded from `total_planet_nitrogen_kg()` because
the recipient reservoir already owns the transferred mass.

`total_ecology_nitrogen_accounted_kg()` remains useful as a terrestrial
boundary audit: active terrestrial stocks plus cumulative exits. Under v2 it is
not expected to stay constant, because fixation and deposition are explicit
inputs from global reservoirs.

## Return processes

### Atmospheric fixation

Each land cell requests reduced fixation according to effective land area,
temperature and current nutrient scarcity:

`2e-6 kgN/m2/day * temperature_factor * (1 - fertility)`.

The sum is capped by the finite atmospheric N2 donor, debited once, then
distributed back to local mineral N. `ecology.nitrogen_fixation_kg_day`
reports the realized extensive rate. This combines biological/abiotic fixation
into one engineering closure; no fixer species or symbiosis is implied.

### Reactive deposition

Fire-emitted reactive N is deposited back to land with a 45-day exponential
relaxation timescale and distributed by represented land area. The donor
reservoir is debited exactly by the amount added to mineral N.
`ecology.nitrogen_deposition_kg_day` reports the realized rate.

### Ocean return

Leached N enters the global ocean dissolved reservoir. A 30-year exponential
denitrification closure transfers dissolved ocean N back to atmospheric N2.
This is a single global box: no river nitrate routing, estuaries, marine
primary production, oxygen limitation or nitrate/ammonium chemistry is
resolved.

## Terrestrial process contract retained from v1

Carbon decomposition drives the fraction of litter/fast/slow organic N that
turns over. N following transferred carbon stays organic; the remainder
mineralizes. Runoff removes only finite mineral N. Plant gross production
requests N using reduced target C:N ratios 25/40/60 for grass/shrub/tree and is
scaled before carbon creation when the mineral donor is insufficient.

Plant turnover/crowding returns associated N to litter. Fire returns
uncombusted mortality to litter, sends 25% of altered N to mineral ash and 75%
to the reactive-atmosphere boundary. Grazed plant N is immediately recycled
65% to litter and 35% to mineral N because persistent fauna N remains outside
this slice.

## LOD and scheduling

All spatial N stocks/rates use ordinary extensive FieldStore split/sum
semantics. The global `NitrogenStore` ignores refine/coarsen events, so focus
LOD cannot duplicate atmosphere/ocean nitrogen. The final nitrogen system runs
after the post-fire/fauna ecology state. Carbon and nitrogen final closures are
resource-independent siblings and do not require an ordering between them.

## Conservation and validation

`total_planet_nitrogen_kg()` counts exactly one copy of each active
terrestrial N stock plus the three global reservoirs. It excludes aggregate
vegetation N, fertility, current rates and cumulative leaching/fire histories.

Regression coverage verifies:

- soil organic/mineral transfer closure and staged decomposition;
- current leaching rate equals its one-day ledger increment;
- finite-N vegetation limitation and uptake debit;
- fire N current-rate/ledger agreement;
- terrestrial loss transfer into ocean/reactive reservoirs;
- positive fixation/deposition under controlled scarcity/emission;
- coupled 30-day planetary N conservation;
- LOD preservation of spatial stocks with invariant global reservoirs; and
- snapshot round-trip/continuation.

The long-run harness reports terrestrial N retention, atmospheric-N2 ratio,
ocean N, cumulative fixation/deposition and
`planet_nitrogen_rel_residual`. `--assert-stable` rejects planetary N drift
above 1e-10 relative.

## Snapshot epoch

Snapshot epoch 31 was the first combined-world format with explicit
terrestrial-N fields. Epoch 32 adds `NitrogenStore`, current boundary rates
and return-flux fields. The store's own snapshot version is 1. Combined epochs
2 through 31 are rejected until explicit migrations exist.

## Explicit limits

Nitrogen v2 does not model:

- resolved atmospheric N2/NOx/NH3 chemistry or transport;
- lightning versus biological fixation pathways;
- fixer species, symbiosis or species-specific nutrient physiology;
- nitrification or explicit terrestrial denitrification;
- river/aquifer nitrate routing or estuarine retention;
- marine primary production or nutrient-limited aquatic food webs;
- phosphorus or other limiting elements;
- microbial biomass/enzyme pools and vertical soil chemistry;
- persistent fauna nitrogen/body stoichiometry;
- calibrated reservoir sizes, fixation/deposition rates or leaching response.

The invariant is conservative ownership: every implemented nitrogen transfer
has one finite donor and one recipient stock; cumulative ledgers remain audit
history rather than substitute reservoirs.

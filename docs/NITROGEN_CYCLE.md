# Nitrogen cycle v3

Nitrogen v1 introduced explicit terrestrial N stocks and finite-N plant
limitation. Planetary nitrogen v2 closed the land boundary with non-spatial
atmospheric/ocean reservoirs and reduced return processes. Nitrogen v3 adds
homeostatic fauna nitrogen so the already authoritative animal carbon biomass
also owns the nitrogen required by its body material. The goal remains
conservative long-run causality, not Earth calibration.

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

Fauna nitrogen is likewise derived rather than independently mutable:
`cohort_nitrogen_kg = cohort_carbon_kg / 4`. The reduced C:N=4 body ratio is
shared by the current herbivore/carnivore demo guilds. Because cohort carbon is
persistent state, this derived fauna N is a persistent material stock without
creating a second cohort degree of freedom that could drift from body biomass.

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
to the reactive-atmosphere boundary.

Fauna retains nitrogen when consumed plant/prey carbon becomes animal biomass.
Final cohort carbon is capped by the N available in the prior body stock plus
consumed food under the fixed body C:N. Assimilated carbon that cannot be
retained under that constraint is added to fauna respiration. N not retained
in final body biomass -- unassimilated dietary N plus N released by net biomass
mortality -- is recycled 65% to litter and 35% to mineral N. Predation applies
the same accounting to prey N before updating carnivore biomass.

## LOD and scheduling

All spatial field N stocks/rates use ordinary extensive FieldStore split/sum
semantics. Fauna N follows `CohortStore`: refine splits cohort count by child
area, coarsen merges count/mass, and migration transfers count. Those existing
operations conserve cohort carbon and therefore conserve the fixed-ratio
derived N without a parallel field. The global `NitrogenStore` ignores
refine/coarsen events, so focus LOD cannot duplicate atmosphere/ocean nitrogen.
The final nitrogen system runs after the post-fire/fauna ecology state. Carbon
and nitrogen final closures remain resource-independent siblings.

## Conservation and validation

`total_planet_nitrogen_kg()` counts exactly one copy of each active
terrestrial field N stock, cohort-derived fauna N and the three global
reservoirs. It excludes aggregate vegetation N, fertility, current rates and
cumulative leaching/fire histories.

Regression coverage verifies:

- soil organic/mineral transfer closure and staged decomposition;
- current leaching rate equals its one-day ledger increment;
- finite-N vegetation limitation and uptake debit;
- herbivore N retention, food-N-limited growth and excess-carbon respiration;
- prey-N transfer into carnivore biomass;
- fire N current-rate/ledger agreement;
- terrestrial loss transfer into ocean/reactive reservoirs;
- positive fixation/deposition under controlled scarcity/emission;
- coupled 30-day planetary N conservation;
- LOD preservation of spatial stocks with invariant global reservoirs; and
- snapshot round-trip/continuation.

The long-run harness reports terrestrial N retention, fauna N, atmospheric-N2
ratio, ocean N, cumulative fixation/deposition and
`planet_nitrogen_rel_residual`. `--assert-stable` rejects planetary N drift
above 1e-10 relative.

## Modeling precedent

The reduced strict-homeostasis contract follows ecological-stoichiometry
practice: consumers retain limiting elements in body material and recycle
material in excess of body requirements. References used for this slice:

- Anderson et al. (2005), *Metabolic Stoichiometry and the Fate of Excess
  Carbon and Nutrients in Consumers*: https://doi.org/10.1086/426598
- May & El-Sabaawi (2022), *Life stage and taxonomy the most important factors
  determining vertebrate stoichiometry*: https://doi.org/10.1002/ece3.9354

The fixed WorldSim C:N=4 is an engineering guild parameter, not a calibrated
claim for any species.

## Snapshot epoch

Snapshot epoch 31 was the first combined-world format with explicit
terrestrial-N fields. Epoch 32 added `NitrogenStore`, current boundary rates
and return-flux fields. Epoch 33 makes cohort biomass own derived fauna N and
changes trophic transfer semantics. Epoch 34 changes authoritative terrestrial
area semantics by replacing coarse cell-center coastal classification with
area-integrated subcell fractions; the nitrogen wire layout is unchanged, but
resuming epoch 33 would reinterpret every land-area-normalized nitrogen process.
The cohort wire layout and `NitrogenStore` version remain unchanged.
Combined epochs 2 through 33 are rejected until explicit migrations exist.

## Explicit limits

Nitrogen v3 does not model:

- resolved atmospheric N2/NOx/NH3 chemistry or transport;
- lightning versus biological fixation pathways;
- fixer species, symbiosis or species-specific nutrient physiology;
- nitrification or explicit terrestrial denitrification;
- river/aquifer nitrate routing or estuarine retention;
- marine primary production or nutrient-limited aquatic food webs;
- phosphorus or other limiting elements;
- microbial biomass/enzyme pools and vertical soil chemistry;
- species/guild-specific variable fauna C:N, explicit protein turnover or
  physiological N excretion pathways;
- calibrated reservoir sizes, fixation/deposition rates, fauna C:N or leaching
  response.

The invariant is conservative ownership: every implemented nitrogen transfer
has one finite donor and one recipient stock; cumulative ledgers remain audit
history rather than substitute reservoirs.

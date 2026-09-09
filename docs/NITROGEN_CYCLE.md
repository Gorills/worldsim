# Nitrogen cycle v1

Nitrogen cycle v1 replaces WorldSim's independent fertility index with a
conservative reduced terrestrial nitrogen boundary. Its purpose is to make
plant production depend on a finite donor nutrient stock and to preserve that
element through the existing soil, vegetation, fire and grazing transfer paths.
It is not an Earth-calibrated C:N biogeochemistry model.

## Authoritative tracked nitrogen

The active terrestrial inventory uses ordinary extensive fields:

- `ecology.litter_nitrogen_kg`;
- `ecology.soil_fast_nitrogen_kg`;
- `ecology.soil_slow_nitrogen_kg`;
- `ecology.mineral_nitrogen_kg`;
- `ecology.grass_nitrogen_kg`;
- `ecology.shrub_nitrogen_kg`;
- `ecology.tree_nitrogen_kg`.

`ecology.vegetation_nitrogen_kg` is a derived sum of the three PFT pools and
must not be counted again. `ecology.soil_fertility` is likewise a diagnostic:
it is a bounded monotonic function of mineral-N density, not an elemental stock.

Two cumulative extensive fields close explicit exits from the active
terrestrial inventory:

- `ecology.nitrogen_leached_kg`;
- `ecology.fire_emitted_nitrogen_kg`.

`total_ecology_nitrogen_accounted_kg()` counts active stocks plus these
boundary ledgers exactly once. Without an implemented aquatic or atmospheric N
recipient, the ledgers preserve conservation and make the missing destination
visible rather than silently deleting nutrient mass.

## Soil turnover and mineralization

`SoilNitrogenModel` consumes the realized carbon decomposition step. For each
litter/fast/slow source pool, the same fraction that decomposes in carbon is
applied to its N stock. N accompanying carbon transferred to the next organic
pool stays organic; the remainder is mineralized.

This deliberately stages transfers: newly moved litter N cannot decompose
again from the fast pool during the same soil step, matching the existing
carbon-time integration contract.

Drainage removes a bounded fraction of the post-mineralization mineral pool.
The reduced leaching response is

`1 - exp(-runoff_depth / 1 m)`.

That scale is an engineering control, not a calibrated nitrate transport
coefficient.

## Plant uptake and limitation

The three reduced PFT target C:N ratios are:

- grass: 25;
- shrub: 40;
- tree: 60.

Potential gross carbon production is computed from the existing climate,
water, light, competition and magic controls. The corresponding N request is
`gross_C / target_C:N`. If the combined request exceeds local mineral N, all
PFT gross production is scaled by the same donor-limited factor before carbon
is created. Realized uptake debits mineral N and is exposed as
`ecology.nitrogen_uptake_kg_day`.

Plant respiration removes carbon but no N in this reduced representation, so
realized tissue C:N can evolve after initialization. Turnover and crowding move
the associated fraction of plant N into litter. A full flexible-stoichiometry
physiology model is outside v1.

## Disturbance and fauna boundary

Fire removes N proportional to the affected vegetation/litter material.
N associated with uncombusted vegetation mortality returns to litter. Of the
remaining altered N, 75% enters the cumulative fire-emission boundary ledger
and 25% returns to local mineral N as reduced ash recycling. These fractions
are engineering parameters and do not represent NOx/N2/NH3 chemistry.

Fauna does not own a persistent N reservoir in v1. When herbivores remove plant
material, its N is immediately returned in the same fauna pass: 65% to litter N
and 35% to mineral N. This keeps the tracked boundary conservative while making
the simplification explicit. Carnivore/prey nitrogen, body stoichiometry,
excretion timing and migration of animal-bound N require a future cohort-N
state extension.

## Initialization and LOD

Initial vegetation N uses the PFT C:N targets. Initial litter, fast-soil and
slow-soil C:N ratios are 40, 14 and 12 respectively. Initial mineral N is a
small regolith-substrate-dependent stock. These values warm-start the reduced
model and are **not calibrated observations**.

All tracked N stocks and ledgers use extensive FieldStore semantics, so
refinement splits them and coarsening sums them. After a cover change,
`ecology.vegetation_nitrogen_kg` and the mineral-density-derived fertility
diagnostic are reconstructed from authoritative component fields.

## Validation contract

The dedicated `worldsim_nitrogen_tests` suite verifies:

- pure soil-N transfer closure and staged organic transfers;
- finite and monotonic mineral-N fertility projection;
- soil-system mineralization/leaching closure;
- production reduction when mineral N is absent;
- exact mineral-N debit for realized plant uptake;
- coupled 30-day nitrogen closure;
- refine/coarsen preservation and aggregate reconstruction; and
- snapshot round-trip/continuation at epoch 31.

Fire and fauna regressions additionally carry internally consistent N fixtures
through their existing carbon-transfer tests. The long-run harness reports
mineral-N density and `tracked_nitrogen_rel_residual`; `--assert-stable`
rejects relative drift above `1e-10`.

## Snapshot epoch

Snapshot epoch 31 is the first combined-world format with authoritative
nitrogen fields and finite-N production semantics. Epoch 30 and older combined
snapshots are rejected until explicit migrations exist.

## Explicit limits

Nitrogen cycle v1 does not model:

- atmospheric N2, fixation, lightning or deposition;
- nitrification, denitrification, ammonia/NOx speciation or gaseous soil loss;
- dissolved nitrate transport into an aquatic nutrient reservoir;
- marine nutrient cycling;
- phosphorus or other limiting elements;
- microbial biomass or enzyme pools;
- vertical soil horizons and vadose-zone chemistry;
- persistent fauna nitrogen/body stoichiometry;
- symbiotic plant traits or species-specific nutrient physiology;
- calibrated fertilization response, C:N ratios or leaching coefficients.

The invariant is narrower and testable: every implemented nitrogen transfer
must name a finite donor and recipient stock, or an explicit cumulative boundary
ledger.

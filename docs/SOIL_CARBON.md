# Soil carbon v1

> Snow-albedo coupling v1 introduced snapshot epoch 21; fauna carbon accounting
> introduced epoch 22, the grazing-timestep correction introduced epoch 23,
> and the magic forcing timebase correction raises the current combined-world
> epoch to 24.
> The Soil-carbon-v1 execution record below remains historical evidence for
> epoch 20.

## Why this slice exists

Before this slice, `ecology.soil` removed a temperature- and moisture-dependent
fraction from `ecology.litter_carbon_kg` every day. No soil stock or boundary
flux received the removed carbon. The effect was observable in the authoritative
state: for an isolated soil step,

```text
litter before > litter after
```

while every other carbon field was unchanged. Long simulations therefore lost
detrital carbon silently, could not retain slow soil memory, and could not audit
whether litter disappeared through decomposition or through a numerical error.
Wildfire v1 made this gap more important because fire now explicitly partitions
combusted fuel between an emission ledger and persistent pyrogenic carbon, while
ordinary decomposition still had neither destination.

The smallest complete correction is a bounded soil-carbon subsystem. It does not
claim an atmospheric carbon cycle, nutrient stoichiometry, microbial community,
or calibrated Earth soil map.

## Compared practice and implementation decision

- USDA's OPUS documentation describes its CENTURY-derived soil organic-matter
  model as first-order decomposition modified by temperature and moisture, with
  material transferred through labile and stabilized pools and part leaving as
  carbon dioxide: https://www.ars.usda.gov/ARSUserFiles/30121500/OPUS/OPUSDocumentation.pdf
- The ORNL DAAC release description for Biome-BGC **4.1.1** couples daily
  weather to persistent vegetation, litter, and soil carbon pools and reports
  their fluxes: https://daac.ornl.gov/MODELS/guides/biome-bgc_guide.html
- Rothamsted's maintained RothC implementation separates decomposable plant
  material, resistant material, microbial biomass, humified organic matter, and
  inert carbon, and exposes temperature and moisture rate modifiers:
  https://github.com/Rothamsted-Models/RothC_Code/blob/main/RothC.for
- The C++ language target is C++20. The implementation uses the standard
  exponential functions defined by N4861 and the repository's existing finite
  numeric boundary: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/n4861.pdf

WorldSim adopts the common stock/flow structure, not the parameterization of any
one reference model. A two-pool reduction is appropriate at the current spatial
and ecological fidelity:

```text
plant turnover / mortality
            |
            v
         litter ---- respiration
            |
            v
       fast soil ---- respiration
            |
            v
       slow soil ---- respiration
```

Adding a third passive pool, explicit microbes, or C:N:P coupling would introduce
new unverified parameters without repairing another current failure mode.

## Authoritative fields

- `ecology.soil_fast_carbon_kg`: extensive active/fast soil organic carbon;
- `ecology.soil_slow_carbon_kg`: extensive stabilized/slow soil organic carbon;
- `ecology.soil_carbon_kg`: extensive derived sum of the two soil pools;
- `ecology.heterotrophic_respiration_kg_day`: extensive carbon flux from the
  most recent soil step;
- `ecology.soil_respired_carbon_kg`: extensive cumulative boundary ledger for
  carbon returned by decomposition.

All five fields split by child area and sum on coarsening. The aggregate is
recomputed after every soil step and cover change; consumers must not add it to
its components when computing a stock total.

The cumulative respiration ledger is intentionally not an atmospheric stock.
It is an audit boundary: coupling it back into atmospheric CO2 requires a future
planetary carbon reservoir and ocean exchange contract.

## Daily transition

For each land cell, start-of-step litter, fast, and slow stocks decay in parallel:

```text
decayed = stock * (1 - exp(-base_rate * modifier * dt))
```

The shared modifier is a bounded Q10 temperature response multiplied by a
root-zone moisture response. The base e-folding times are 2.5 years for litter,
4 years for fast soil carbon, and 80 years for slow soil carbon. These are reduced
model controls, not calibrated claims.

Transfers are:

- litter decomposition: 55% to fast soil carbon, 45% respiration;
- fast-pool decomposition: 35% to slow soil carbon, 65% respiration;
- slow-pool decomposition: 100% respiration.

Only start-of-step stocks decompose. Incoming material cannot cross multiple
pools in one step, which avoids order dependence and excessive large-step decay.
The exponential update keeps every requested decay bounded by its source stock.

For one isolated soil step the executable accounting contract is exact up to
floating-point rounding:

```text
litter_before + fast_before + slow_before + respired_ledger_before
  = litter_after + fast_after + slow_after + respired_ledger_after
```

When a cell has no effective land area, existing litter and soil carbon are
retained and decomposition is dormant. Silently deleting those stocks during a
temporary land-mask transition would violate the same contract; sediment burial
or marine export needs an explicit future destination.

## Initialization and fertility coupling

Initial fast and slow soil stocks are deterministic functions of effective land
area, regolith substrate, and climate suitability. Their purpose is to
provide a non-zero memory state for the reduced model, not to reconstruct a
historical spin-up. Production calibration should replace this initialization
with a spin-up or data assimilation workflow.

The existing dimensionless fertility index retains litter as its labile organic
substrate signal. Decomposition of all three pools contributes a small
mineralization pulse, so persistent soil carbon affects fertility through
turnover rather than being treated as immediately available. No elemental
nitrogen or phosphorus conservation is claimed.

## Persistence and validation

The added authoritative fields change the field schema and continuation
semantics, so Soil carbon v1 uses snapshot epoch **20**. Epochs 2-19 remain
strictly rejected; no migration is supplied.

Regression coverage must verify:

- per-step soil carbon closure at short and long timesteps;
- warm/moist acceleration relative to cold/dry conditions;
- no decomposition of newly transferred carbon in the same step;
- dormant preservation without effective land area;
- component and ledger conservation through refine/coarsen;
- aggregate reconstruction after cover changes;
- epoch-20 snapshot round-trip and deterministic continuation.

## Executed evidence — 2026-09-08

- GCC 13.3 RelWithDebInfo build and complete CTest passed: **12/12** in
  59.11 seconds.
- `worldsim_soil_carbon_tests` passed pure-model closure, environmental
  response, invalid-input rejection, staged-transfer, short/long-step,
  submerged-stock, LOD and snapshot-continuation scenarios.
- ASan + UBSan + float-cast-overflow instrumentation passed the dedicated
  soil-carbon suite, bounded smoke, and all 14 audit scenarios including the
  three-seed annual continuation. Leak detection remained disabled for the
  tracing environment as documented by the audit.
- Godot **4.7.2.stable.mono.official.ed1daf0bf** rebuilt against the pinned
  godot-cpp **10.0.0-rc2** / API 4.7 target. `ci_smoke.gd` verified aligned,
  finite soil fields and the aggregate/component identity; the Russian
  `ci_simulation_lab.gd` run exposed all **67** fields and completed at tick 48.
- English and Russian gettext catalogs passed `msgfmt --check`; their existing
  missing-maintainer-metadata warnings remain unchanged.
- Clang is **NOT VERIFIED** for this slice because the compiler executable is
  not installed in the local environment.

## Explicit limits

There is no atmospheric CO2 reservoir or radiative feedback, dissolved/eroded
organic carbon transport, peat/wetland anaerobic pathway, permafrost, vertical
soil profile, microbial biomass, priming, or C:N:P limitation. Fauna carbon is
also outside this soil accounting boundary. Parameters and initial stock
densities are **NOT CALIBRATED** against observations. Soil carbon v1 establishes
conservative state transitions and persistent memory on which those later slices
can be built.

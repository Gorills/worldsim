# Carbon cycle v1

Carbon cycle v1 closes the reduced WorldSim fast-carbon boundary across
terrestrial ecology, the atmosphere and a global ocean reservoir. Its purpose
is authoritative conservation and climate feedback at the current simulation
resolution; it is not a calibrated Earth-system carbon model.

## Authoritative stocks

`ClimateStore` owns two global carbon stocks independently of adaptive focus
LOD:

- atmospheric carbon, initialized at 280 ppm CO2;
- ocean carbon, initialized at 38,000 PgC.

The ppm conversion uses 2.12 PgC per ppm CO2. NOAA GML documents this
conversion, while IPCC assessments place the ocean reservoir near 38,000 PgC:

- NOAA GML, CO2 FAQ: https://gml.noaa.gov/ccgg/about/co2_measurements.html
- IPCC AR6 WGIII, Chapter 12: https://www.ipcc.ch/report/ar6/wg3/chapter/chapter-12/

Spatial ecology stocks remain ordinary extensive fields: grass, shrub, tree,
litter, fast soil carbon, slow soil carbon and pyrogenic carbon. Fauna cohort
carbon is derived from cohort count and wet mass. Aggregate vegetation/soil
fields are diagnostics and are not counted again.

Cumulative `ecology.soil_respired_carbon_kg`,
`ecology.fauna_respired_carbon_kg` and
`ecology.fire_emitted_carbon_kg` remain audit ledgers. They are not reservoirs
and are deliberately excluded from `total_planet_carbon_kg()`.

## Daily land-atmosphere closure

The final daily `ecology.carbon_cycle` system runs after vegetation, optional
fire and optional fauna. It integrates the realized extensive rates

`heterotrophic respiration + fauna respiration + fire emission - NPP`

over the same elapsed daily interval. Positive values add carbon to the
atmosphere; NPP removes carbon from it. Fire and fauna expose current rate
fields in addition to their existing cumulative ledgers so the carbon system
never infers a flux by differencing history state.

The atmospheric donor is never silently clamped. A transition that would
overdraw it is rejected as an invalid model state.

## Air-sea exchange

The ocean closure is a conservative two-box relaxation. Exchange is driven by
the difference between atmospheric and ocean inventories normalized by their
reference inventories. A single transfer is computed and applied
antisymmetrically, so atmosphere + ocean carbon is unchanged by air-sea
exchange.

The 180-year relaxation time is an engineering parameter chosen for a slow
global ocean buffer. It is not a claim about resolved mixed-layer chemistry,
deep-ocean circulation or carbonate-system equilibration. Reduced-complexity
climate models commonly represent carbon uptake with reservoir/box dynamics;
future calibration should replace this closure rather than layer a second
carbon owner on top of it.

## CO2 climate feedback

Atmospheric carbon projects `climate.atmospheric_co2_ppm`. The climate energy
step adds the logarithmic CO2 forcing

`5.35 * ln(C / 280 ppm) W/m2`

uniformly to the reduced surface energy budget and accumulates the contribution
in `ClimateBudget::co2_forcing_j`. The coefficient follows the classic
Myhre et al. reduced forcing relation:

- Myhre et al. (1998), *New estimates of radiative forcing due to well mixed
  greenhouse gases*: https://doi.org/10.1029/98GL01908

This is radiative forcing, not a spatial greenhouse-gas transport model.
WorldSim does not currently resolve atmospheric chemistry, carbon isotopes,
cloud-adjusted effective forcing, ocean carbonate chemistry or a biological
ocean pump.

## Conservation contract

`total_planet_carbon_kg()` counts exactly one copy of each authoritative
fast-carbon stock:

- atmosphere;
- ocean;
- grass/shrub/tree;
- litter;
- fast/slow soil carbon;
- fauna cohort carbon;
- pyrogenic carbon.

The daily coupled regression checks this inventory across the complete default
scheduler. Climate tests separately verify conservative air-sea exchange,
280 -> 560 ppm doubling, logarithmic forcing, energy-ledger inclusion and
snapshot continuation.

Snapshot epoch 30 is the first combined-world format with authoritative carbon
reservoirs. `ClimateStore` state version 3 serializes both carbon reservoirs
and the cumulative CO2 forcing-energy ledger. Older combined snapshots are
rejected until an explicit migration exists.

## Explicit limits

Not modeled in v1:

- fossil-fuel, carbonate-rock or mantle carbon;
- weathering/subduction/volcanic carbon fluxes;
- dissolved inorganic-carbon chemistry, alkalinity or pH;
- explicit surface/deep ocean boxes or circulation;
- marine primary production/export;
- land-use emissions or a human emissions driver;
- nitrogen/phosphorus coupling;
- Earth-calibrated sink fractions or transient climate response.

Those are future bounded slices. The current contract is conservation first:
new carbon processes must identify a donor and recipient stock or an explicit
external boundary flux.

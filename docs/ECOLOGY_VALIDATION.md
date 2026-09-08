# Ecology validation and scope

WorldSim's ecology is a reduced planet-scale vertical slice. It is intended to make the world biologically stateful and to exercise coupling between geology, climate, water, vegetation and cohort fauna. It is **not** a calibrated Earth ecosystem model.

## Living-soil v1 contract

The ecology pipeline now follows this scheduler order when the default modules are present:

```text
climate.surface
    -> ecology.hydrology
    -> geology.evolution
    -> ecology.soil
    -> ecology.vegetation
    -> ecology.fauna
```

This ordering gives geology the current terrestrial runoff, gives soil the current regolith state, and gives vegetation the resulting soil state.

### Root-zone water storage

`hydrology.soil_water_m3` remains an extensive water stock, but its local storage capacity is no longer a fixed water depth on every land cell. The capacity depth rises from a small fractured-substrate store toward a bounded developed-soil store as `geology.regolith_thickness_m` increases.

The exact WorldSim closure is an engineering approximation for the coarse cells. It does not resolve soil texture, hydraulic conductivity, horizons, groundwater or an unsaturated-flow profile.

Landlab's soil-moisture component provides the relevant modeling precedent: root-zone moisture, runoff/leakage and vegetation water stress are treated as linked ecosystem state, and the component exposes root depth, porosity and field-capacity parameters rather than assuming one universal water bucket.

- Landlab, `SoilMoisture`: https://landlab.readthedocs.io/en/latest/generated/api/landlab.components.soil_moisture.soil_moisture_dynamics.html
- Laio, F., Porporato, A., Ridolfi, L. & Rodriguez-Iturbe, I. (2001), *Plants in water-controlled ecosystems: active role in hydrologic processes and response to water stress II*. https://doi.org/10.1016/S0309-1708(01)00005-7

### Soil fertility and detritus

Two persistent ecology fields close the first living-soil feedback:

- `ecology.soil_fertility`: intensive reduced fertility state in `[0,1]`;
- `ecology.litter_carbon_kg`: extensive detrital carbon stock.

Regolith depth provides the mineral-substrate contribution to fertility. Plant turnover, herbivory waste and a reduced carcass-carbon return add litter. Litter decomposes faster under warm/moist conditions and feeds back into the reduced fertility state. Strong runoff leaches that state.

This deliberately stops short of an elemental nitrogen/phosphorus budget. `soil_fertility` is an index, not kilograms of N or P, and litter decomposition may return fertility while carbon leaves the modeled terrestrial pools. A later biogeochemistry slice should introduce explicit nutrient reservoirs before any claim of elemental conservation.

The conceptual basis is standard ecosystem-process practice: soil organic matter depends strongly on climate and substrate controls, while process models such as Biome-BGC couple water, vegetation, litter and soil state rather than treating plant productivity as independent of the substrate.

- Parton, W. J., Schimel, D. S., Cole, C. V. & Ojima, D. S. (1987), *Analysis of Factors Controlling Soil Organic Matter Levels in Great Plains Grasslands*. https://doi.org/10.2136/sssaj1987.03615995005100050015x
- Thornton, P. E. et al. (2002), *Modeling and measuring the effects of disturbance history and climate on carbon and water budgets in evergreen needleleaf forests*. https://doi.org/10.1016/S0168-1923(02)00108-9
- ORNL DAAC Biome-BGC model description: https://daac.ornl.gov/MODELS/guides/biome-bgc_manuscript.html

### Vegetation and fauna coupling

Vegetation NPP is now limited by the living-soil fertility state in addition to temperature, solar forcing, root-zone water and magic growth forcing. Vegetation turnover transfers carbon into litter instead of allowing all biomass loss to disappear from the terrestrial organic-matter loop.

The cohort fauna model still uses two reduced functional groups. A fraction of consumed plant carbon returns to litter, and a bounded estimate of killed wet biomass returns as carcass carbon. This is an ecological feedback only; cohort body mass is not yet a conserved carbon store.

## Executable validation

`worldsim_tests` now checks:

- soil water storage differs between bare/thin regolith and deep regolith under the same forcing;
- low-fertility and high-fertility copies of the same world produce different vegetation NPP through the production scheduler;
- vegetation turnover creates litter;
- a litter-rich copy of the same soil increases reduced fertility relative to a litter-free copy;
- fertility remains finite and in `[0,1]`, while litter, vegetation, water and cohort counts remain finite and non-negative;
- extensive litter carbon participates in the existing LOD split/sum semantics through the field store;
- snapshot determinism and continuation include the new ecology state through the field schema.

## Explicitly unsupported ecology claims

Living-soil v1 does **not** yet provide:

- explicit nitrogen, phosphorus or other elemental nutrient conservation;
- microbial biomass, soil horizons, texture classes or soil chemistry;
- groundwater and vadose-zone flow;
- plant functional types, succession or seed dispersal;
- species-specific physiology, genetics or evolution;
- fauna migration, habitat selection or seasonal movement;
- aquatic food webs;
- disease, parasites or decomposer cohorts;
- closed global carbon accounting including atmosphere/ocean reservoirs;
- Earth-calibrated productivity, decomposition or carrying-capacity parameters.

These are future domain slices. Human settlement/population simulation should wait until the non-human biosphere can persist, spread and recover under the same authoritative world contracts.

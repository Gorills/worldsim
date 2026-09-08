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

### Flora v1: functional types, succession and local dispersal

Terrestrial vegetation is split into three persistent extensive carbon pools:

- `ecology.grass_carbon_kg`;
- `ecology.shrub_carbon_kg`;
- `ecology.tree_carbon_kg`.

`ecology.vegetation_carbon_kg` remains the authoritative aggregate exposed to existing consumers and is recomputed as the sum of the three pools after vegetation and fauna updates.

Each functional type has a different reduced temperature/moisture/fertility niche, productivity scale, turnover rate, maximum biomass density and local-establishment strength. Grass establishes and turns over fastest, shrubs are intermediate, and trees establish more slowly but persist longer. Woody biomass suppresses lower strata through reduced light/space access, so differential growth plus turnover produces a reduced succession process rather than three independent carbon buckets.

Recruitment is propagule-limited. A cell with no local biomass can establish a functional type only if an active neighboring region already contains that type. Neighbor source density is sampled from the start-of-step vegetation state through `WorldState::active_neighbors4()`; coarse/fine interfaces therefore use the same normalized adaptive-cover weights as geology. This also prevents iteration-order artifacts in which a new recruit would seed another cell again during the same daily step.

This is local grid-neighbor dispersal, not an explicit seed-bank or distance kernel. There is no spontaneous plant generation when all three plant pools are zero.

The representation follows the large-scale simplification used by dynamic global vegetation models: broad plant functional types compete for resources/space while establishment, mortality/turnover and changing relative cover approximate vegetation dynamics. Reviews of DGVM design describe PFT-based competition/establishment as a standard tractability tradeoff, while LPJ couples PFT competition with daily water/carbon processes and slower vegetation dynamics.

- Fisher et al. (2014), *Plant functional types in Earth system models*: https://pmc.ncbi.nlm.nih.gov/articles/PMC4071098/
- Sakschewski et al. / DGVM process review, *Dynamic Global Vegetation Models: Searching for the balance between demographic process representation and computational tractability*: https://journals.plos.org/climate/article?id=10.1371/journal.pclm.0000068
- Sitch et al. (2003), LPJ Dynamic Global Vegetation Model: https://doi.org/10.1046/j.1365-2486.2003.00569.x

### Vegetation and fauna coupling

Total vegetation NPP remains limited by living-soil fertility, temperature, solar forcing, root-zone water and magic growth forcing, but production and turnover are now resolved per functional type. Turnover returns carbon to litter.

The reduced herbivore cohorts preferentially consume grass, then shrubs, with a smaller tree forage contribution. Fauna removes carbon from the same PFT pools and then recomputes total vegetation, preventing the aggregate field from drifting away from functional-type state. A fraction of consumed plant carbon returns to litter, and a bounded estimate of killed wet biomass returns as carcass carbon. Cohort body mass is still not a conserved carbon store.

### Fauna v2: habitat selection and local migration

After the daily local feeding/predation update, the fauna system computes a reduced habitat-quality field for each trophic group. Herbivore quality rises with preference-weighted plant forage per effective land area; carnivore quality rises with herbivore biomass density. Each cohort compares its current cell with the four adjacent spatial regions and redistributes only toward a side whose area-weighted quality is materially better.

Movement uses the same `WorldState::active_neighbors4()` mixed-LOD contract as geology and plant dispersal. If the preferred neighboring region is represented by multiple fine cells, movers are distributed across those active cells according to adaptive-cover weight multiplied by local habitat quality.

Migration is applied from a frozen post-feeding cohort snapshot. Arriving animals therefore cannot immediately move again in the same fauna tick, making results independent of cohort-map iteration order. `CohortStore::transfer_count()` performs the actual redistribution so `Cohort::cell` and the store's spatial index cannot diverge. A destination cohort of the same lineage/species/functional group is merged rather than duplicated.

This is a reduced population redistribution model, not a trajectory-level movement model. The design follows the core movement-ecology principle that movement capacity/state and environmental resource selection are coupled. Integrated step-selection methods likewise treat movement and resource selection jointly rather than as independent processes.

- Nathan et al. (2008), *A movement ecology paradigm for unifying organismal movement research*: https://doi.org/10.1073/pnas.0800375105
- Avgar et al. (2016), *Integrated step selection analysis: bridging the gap between resource selection and animal movement*: https://doi.org/10.1111/2041-210X.12528

## Executable validation

`worldsim_tests` now checks:

- soil water storage differs between bare/thin regolith and deep regolith under the same forcing;
- low-fertility and high-fertility copies of the same world produce different vegetation NPP through the production scheduler;
- vegetation turnover creates litter;
- a litter-rich copy of the same soil increases reduced fertility relative to a litter-free copy;
- mixed-LOD active-cover weights close to one and resolve both fine-neighbor composites and coarse ancestors;
- grass/shrub/tree carbon remains non-negative and sums to total vegetation after vegetation/fauna updates;
- a globally sterile plant cover remains sterile without propagules;
- neighboring grass establishes into an empty suitable cell;
- woody canopy suppresses grass relative to an otherwise identical open cell;
- indexed cohort transfer preserves total count, keeps source/target spatial lookup coherent, and merges repeated transfers into an existing same-lineage destination cohort;
- herbivores partially redistribute from low-forage habitat toward a neighboring high-forage cell;
- carnivores partially redistribute toward neighboring prey biomass;
- migrants do not take a second spatial step during the same fauna tick;
- fertility remains finite and in `[0,1]`, while litter, vegetation, water and cohort counts remain finite and non-negative;
- extensive litter carbon participates in the existing LOD split/sum semantics through the field store;
- snapshot determinism and continuation include the new ecology state through the field schema.

## Explicitly unsupported ecology claims

The current ecology slice does **not** yet provide:

- explicit nitrogen, phosphorus or other elemental nutrient conservation;
- microbial biomass, soil horizons, texture classes or soil chemistry;
- groundwater and vadose-zone flow;
- species-level plant physiology, explicit seed banks, long-distance dispersal kernels or evolutionary adaptation;
- species-specific physiology, genetics or evolution;
- explicit wildfire, storm/flood mortality, grazing-driven state transitions or other disturbance regimes;
- individual trajectories, home ranges, movement memory, explicit barriers, long-distance dispersal or seasonal migration;
- aquatic food webs;
- disease, parasites or decomposer cohorts;
- closed global carbon accounting including atmosphere/ocean reservoirs;
- Earth-calibrated productivity, decomposition or carrying-capacity parameters.

These are future domain slices. Human settlement/population simulation should wait until the non-human biosphere can persist, spread and recover under the same authoritative world contracts.

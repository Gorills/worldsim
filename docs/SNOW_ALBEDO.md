# Snow-albedo coupling v1

## Decision and reproduced failure

Before this slice, basin hydrology already stored snow water as an extensive,
conservative field, but `climate.surface` used a fixed land albedo of 0.28.
Two otherwise identical worlds therefore absorbed exactly the same shortwave
energy whether their land carried no snow or 0.10 m snow-water equivalent.
An executable regression reproduced the failure with
`snow water did not reduce absorbed shortwave radiation` before the climate
implementation changed.

That missing edge also let grass photosynthesize under complete snow cover and
allowed an active fire to retain danger on snow-covered ground. The chosen
slice closes those three existing-domain interactions. It does not introduce a
standalone cryosphere, sea ice, glaciers, snow thermodynamics or a calibrated
land-surface scheme.

Affected contracts are climate/hydrology scheduler resources, reference-grid
aggregation, radiation accounting, vegetation/fire forcing, field discovery,
snapshots and Godot diagnostics. The field schema and climate continuation
state change, so the strict global snapshot epoch advances from 20 to **21**;
`ClimateStore` advances from chunk version 1 to 2.

## References read before implementation

- NASA's snow science overview describes the observed high reflectivity of
  snow and the snow-albedo feedback: https://snow.nasa.gov/science
- The versioned **CLM5.0 / CESM2.1** technical note keeps snow mass as state,
  derives fractional snow-covered area, and uses that fraction in surface
  albedo and flux calculations:
  https://escomp.github.io/CTSM/release-clm5.0/tech_note/Snow_Hydrology/CLM50_Tech_Note_Snow_Hydrology.html
- CLM5.0 blends snow and snow-free ground albedos by snow-covered fraction:
  https://escomp.github.io/CTSM/release-clm5.0/tech_note/Surface_Albedos/CLM50_Tech_Note_Surface_Albedos.html
- CLM5.0 also reduces exposed vegetation area when snow buries it, with short
  vegetation buried more readily than trees and shrubs:
  https://escomp.github.io/CTSM/release-clm5.0/tech_note/Ecosystem/CLM50_Tech_Note_Ecosystem.html

These are comparable land-model practices, not dependencies or evidence of
WorldSim fidelity. CLM has layered snow, accumulation/depletion hysteresis,
subgrid topography, spectral radiative transfer and canopy geometry that do not
exist here. WorldSim deliberately uses the smallest closure supported by its
current state.

## Representation and LOD contract

`hydrology.snow_water_m3` remains the only snow stock. Climate does not copy or
own that water. At the beginning of each climate step, every active leaf maps
to its fixed `ClimateStore` base-level ancestor and contributes its extensive
snow volume exactly once. Fine descendants are therefore summed without
allocating a separate adaptive traversal for every reference node. Simulation
focus never coarsens below the configured base/reference level. This keeps the
result independent of focus LOD and never treats `neighbors4()` as an
adaptive-cover query.

Snow-water equivalent over the reference node's fixed land area is converted
to fractional cover by the bounded reduced closure

```text
snow_cover = 1 - exp(-SWE / 0.02 m)
```

The 0.02 m scale is an engineering parameter, not a fitted CLM parameter. A
stateless curve is used because WorldSim has no snow age, maximum-accumulation
history, snow density, layer structure or subgrid relief state with which to
implement CLM's hysteretic depletion curve honestly.

The projected intensive fields are:

- `climate.snow_cover_fraction`, in `[0,1]`;
- `climate.surface_albedo`, the fixed-reference land/ocean mixture actually
  used by the reduced energy model, in `[0,1]`.

They are diagnostics, not additional stocks. The reference snow-cover value is
stored in `ClimateStore` so snapshot round trips preserve the exact
continuation/diagnostic state; the authoritative water remains only in the
hydrology field.

## Coupling and numerical boundary

Land albedo is blended linearly between the existing snow-free value 0.28 and
the reduced snow value 0.75. Ocean albedo remains 0.30:

```text
land_albedo = 0.28 + snow_cover * (0.75 - 0.28)
absorbed_shortwave = insolation * (1 - land_albedo)
```

The actual absorbed energy still enters the existing cumulative solar ledger,
so the climate heat identity remains
`heat = initial + absorbed shortwave - outgoing longwave` apart from floating
point roundoff. Climate observes the snow state left by the previous hydrology
operation; snowfall or melt produced later in the current scheduler pass feeds
back on the next base tick. This explicit one-tick operator split avoids a
hidden iterative land/atmosphere solve.

Vegetation multiplies gross production by PFT exposure factors. Complete cover
leaves exposure 0.0 for grass, 0.50 for shrubs and 0.85 for trees; respiration
and turnover continue to draw from their bounded carbon pools. This represents
relative burial only, not canopy radiative transfer or phenology. Fire danger
is multiplied by the snow-free fraction; complete projected cover therefore
suppresses burning in this reduced cell-scale model.

## Validation contract

Executable coverage must demonstrate:

- deep snow lowers absorbed shortwave energy and projects high cover/albedo;
- the energy ledger still closes with dynamic albedo;
- reference snow cover and temperature are invariant to focus-only refinement;
- complete cover suppresses grass production and fire danger;
- malformed persisted snow-cover state is rejected without mutating the live
  store;
- current epoch-25 snapshot round trip and continuation are deterministic;
- generic C ABI/Godot discovery exposes finite cover and albedo fields.

## Explicit limits

There is no snow age or density, fresh/old snow distinction, forest masking,
impurity effect, spectral albedo, snow thermal insulation, sublimation,
permafrost, glacier mass balance, sea ice, or subgrid elevation distribution.
The albedos and SWE scale are **NOT CALIBRATED**. The implementation establishes
a conservative, LOD-stable causal feedback around the already-existing snow
stock; it does not claim cryosphere-model fidelity.

## Executed evidence — 2026-09-08

- GCC RelWithDebInfo build and the complete CTest suite passed: **12/12** in
  53.52 seconds on the final run.
- The climate suite passed the pre-fix radiation regression, exact reduced
  closure endpoints, dynamic-albedo energy accounting, projected diagnostics,
  focus-LOD invariance, grass burial, malformed-state isolation and snapshot
  continuation. The fire suite passed complete-cover suppression.
- ASan, UBSan and float-cast-overflow instrumentation passed the climate/fire
  suites, bounded smoke and all 14 annual audit scenarios. Leak detection was
  disabled for the tracing environment as in the earlier audit.
- The 730-day, seed-42, level-2 adaptive diagnostic closed planet water to
  `1.68148e-10` relative and surface energy to `1.20263e-12` relative. Final
  reference snow cover ranged from 0 to approximately 1, with a land-area mean
  of 0.1613; these are inspectability results, not observational validation.
- In the focused 30-day CLI scenario, replacing one adaptive-cover traversal
  per reference node with one active-cell accumulation pass reduced a same-host
  single run from 23.20 to 15.11 seconds. This is an implementation comparison,
  not a target-hardware performance claim.
- Godot **4.7.2.stable.mono.official.ed1daf0bf** rebuilt against pinned
  godot-cpp **10.0.0-rc2** / API 4.7. Native smoke discovered 69 finite fields;
  the Russian 512 x 256 laboratory integration completed at tick 48.
- English and Russian catalogs passed `msgfmt --check`. Their pre-existing
  missing-maintainer metadata warnings remain unchanged.

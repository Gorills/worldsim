# Terrain vertical slice

This document records the architecture and performance decisions for the first walkable procedural world slice.

## Scope

The slice intentionally contains only terrain presentation:

- stateful planet-scale geological elevation reconstructed from the simulation cover;
- bounded sphere-native procedural sub-cell detail over that reconstructed macro relief;
- no rendered or simulated water;
- no rendered rivers or other hydrological surface features;
- no climate, hydrology, vegetation, fauna, settlements, or other gameplay domains in the Godot terrain scene;
- a first-person walker over streamed terrain chunks.

The existing full default simulation remains available for kernel/domain tests and the diagnostic adapter API.

## Authority and coordinate model

The authoritative spatial model remains the existing cube-sphere hierarchy. No second flat world model is introduced.

`TerrainGenerator` is a deterministic core service keyed by world seed. It owns a `TectonicModel` and can be sampled either from a unit direction on the planet or from local projected meter coordinates. Current authoritative geography uses persistent `GeologyModel` state. The walking height/patch APIs now reconstruct that adaptive `geography.elevation_m` field and retain `TerrainGenerator` only for deterministic sub-cell presentation detail. The global elevation map continues to read the actual active geography field without that detail, while tectonic debug layers show the seed model.

The walking projection is azimuthal-equidistant and centered at 45° N, 70° E, but it is not part of terrain generation. `sample_projected()` converts local walker meters to a unit direction and delegates to `sample_direction()`. The static preview sampler uses `TectonicModel::macro_elevation_m` as its broad hypsometric base. The previous fixed anisotropic continent cap no longer participates in generation. Existing sphere-native FBM remains only as bounded 180 km rolling relief and 900 m local detail, plus a bounded ridged orogenic term modulated by tectonic uplift and continental affinity. Final `land_fraction` is derived from final elevation across a narrow sea-level transition.

A regression samples 1,024 sphere directions and requires procedural preview elevation to correlate above 0.95 with tectonic macro relief while retaining measurable detail and keeping the detail residual below 1.5 km. A separate level-5 cube-sphere integration keeps seed-42 global land coverage in a broad non-degenerate range and preserves dry land at the existing local-walker origin.

### Authoritative walking-surface reconstruction

Simulation LOD and rendering LOD remain separate. At the configured maximum
simulation level, cells near the seed-42 walker origin are still roughly 59 km
in characteristic size, while a walking mesh vertex is spaced 8 m apart. Direct
piecewise-constant sampling would therefore turn the active cover into broad
terraces. Raising simulation LOD to render resolution would incorrectly make a
presentation requirement own the authoritative world budget.

The adapter instead evaluates

```text
render_height(p) = R(geography.elevation_m, p)
                 + TerrainGenerator(p) - R(TerrainGenerator anchors, p)
```

`R` is a normalized compact-support reconstruction over virtual cells at the
configured maximum simulation level. Each virtual-cell value is first resolved
through `WorldState::resolve_active_cover()`, so a coarse active ancestor or
active descendants are combined through the existing area-weighted contract.
The renderer never treats same-level `neighbors4()` results as if they were
active leaves. Positive normalized weights keep the reconstructed macro surface
inside the range of its contributing cell values. The second pair of terms is a
high-pass residual: it preserves deterministic local shape without replacing
the evolving simulation height with the immutable seed terrain.

The compact weight is the standard Wendland C2 form
`(1-r)^4 * (1+4r)` for normalized radius `r < 1`. It is a presentation
reconstruction, not a conservative simulation prolongation: stored cell values,
mass budgets, erosion routing, snapshots and all other domain consumers remain
unchanged. Resolving the adaptive cover before reconstruction follows the same
coarse/fine separation used in established AMR practice:

- H. Wendland (1995), *Piecewise polynomial, positive definite and compactly
  supported radial functions of minimal degree*:
  https://doi.org/10.1007/BF02123482
- Berger & Colella (1989), conservative coarse/fine AMR synchronization:
  https://doi.org/10.1016/0021-9991(89)90035-1

The adapter exposes a transient terrain revision derived from active cell ids
and authoritative elevation values. A revision change dirties existing chunks.
The chunk under the player updates its `ArrayMesh` and `HeightMapShape3D`
synchronously; remaining visible chunks update at one per frame. Render and
collision always use the same sampled height array. Godot 4.7 documents the
relevant mutable height-map data and procedural mesh contracts:

- https://docs.godotengine.org/en/4.7/classes/class_arraymesh.html
- https://docs.godotengine.org/en/4.7/classes/class_heightmapshape3d.html

The reconstruction cost and dirty-chunk update budget on target player hardware
remain **NOT VERIFIED** until profiled outside CI.

This follows the same relevant large-planet practice as Demiurge: terrain is a deterministic function of seed plus spherical position, and its macro pipeline is angular/normalized rather than derived from a global flat map. WorldSim uses its own analytical tectonic model and does not copy Demiurge's implementation:

- https://github.com/owenyuwono/demiurge

After simulation-cover refinement/coarsening, derived geography is recomputed from retained geology state through the module lifecycle. The state is not reset to seed terrain. The walker reconstruction consumes that derived field without taking ownership of it.

Because authoritative geography semantics are part of persistent world state, snapshot compatibility advances whenever that terrain/geology contract changes. Tectonic authority introduced version 3, orogenic shaping version 4, plate-layout diversification version 5, the minimum-separation correction version 6, stateful geological evolution version 7, burial-dependent sediment compaction version 8, separated fluvial/hillslope geomorphology version 9, depth-dependent regolith production version 10, critical-slope hillslope acceleration version 11, and coast-to-basin marine sediment routing version 12. Global snapshot version 13 added persistent living-soil ecology fields; version 14 added persistent grass/shrub/tree functional-type pools and propagule-limited vegetation semantics; version 15 added habitat-selected fauna redistribution. Version 16 corrects crust restriction/buoyancy, seasons and vegetation loss accounting, version 17 adds persistent basin hydrology, version 18 adds persistent coupled climate heat/moisture state, version 19 adds authoritative wildfire state, version 20 adds persistent fast/slow soil carbon and respiration accounting, and version 21 adds snow-cover/albedo feedback state. At epoch 21, versions 2 through 20 were rejected by the authoritative-world snapshot contract.

Fauna carbon accounting subsequently introduced epoch 22. The grazing-timestep
correction introduced epoch 23. The magic forcing timebase correction introduced
epoch 24, and the wildfire burn-cap timebase correction introduces epoch 25. The
current reader therefore rejects global snapshot versions 2 through 24; the
preceding history through epoch 24 is retained to show which terrain-era
states they contain.

## Godot large-world strategy

The stock Godot project remains a normal single-precision build. Logical projected coordinates are kept as GDScript scalar values while scene-tree coordinates are periodically shifted back near the origin. Terrain chunk transforms are rebuilt relative to that logical origin. Scene cardinal axes follow Godot's right-handed convention: +X is east and -Z is north, so projected north is reflected into negative scene Z before rendering or movement is compared with a north-up map.

This follows Godot's documented precision limits for large single-precision worlds and uses origin shifting instead of requiring a custom double-precision engine build:

- https://docs.godotengine.org/en/4.7/tutorials/physics/large_world_coordinates.html
- https://docs.godotengine.org/en/4.7/tutorials/assets_pipeline/importing_3d_scenes/model_export_considerations.html

The initial shift threshold is 1,024 m, comfortably inside the range where Godot documents high positional precision for first-person gameplay.

## Terrain streaming

Near collision and distant rendering use separate LOD contracts. The existing
walking chunks remain the only physics terrain:

- chunk size: 256 m;
- mesh resolution: 33 x 33 vertices;
- vertex spacing: 8 m;
- visible radius: 3 chunks;
- retained radius: 4 chunks;
- at most one missing or dirty near chunk is generated per rendered frame;
- the chunk under the initial player position is built synchronously before movement starts.

For the fixed seed-42 walking demonstration, startup placement is intentionally
inside the existing mountain regression region instead of the projection origin.
The viewer samples the same 40 km x 40 km reconstructed-terrain window centered
at east 5,573 km / north -1,800.3 km, finds its highest 625 m-grid sample, then
starts at the lowest dry sample between 8 km and 12 km from that peak and faces
it. This keeps the diagnostic mountain large enough in the first-person view to
judge silhouette, shading and LOD detail without exaggerating authoritative
height. Terrain authority, simulation focus, snapshots and the reconstruction
formula are unchanged.

The walker now also renders visual-only nested spherical terrain rings. Every
ring is a 65 x 65 regular grid, sample spacing doubles from 64 m through
16,384 m, and the outer half-extent therefore grows from 2.048 km to
524.288 km. The camera far plane remains 400 km; the final ring deliberately
extends beyond it so the visible horizon never reaches the mesh boundary. The
denser grid keeps 512 m sampling out to 16.384 km and 1,024 m sampling out to
32.768 km instead of collapsing mountain-scale relief into 2,048 m samples at
those ranges. Heights come from the same reconstructed terrain adapter used by
walking collision. The regional baseline remains authoritative geology; the
adapter then adds deterministic presentation-only orography below the adaptive
cell scale in convergent dry-land belts. Broad 60/28/16 km ridge and summit fields
place the mountain mass; an additional signed 7.5/3.2 km crag/gully field is gated
by that mountain mass to cut local ribs and drainage-like gullies without adding
generic roughness away from uplift belts. That visual/collision relief is bounded,
sphere-native and never mutates simulation fields, conserved stores or snapshots.
The adapter converts the sampled sphere directions to a player-local tangent
frame in double precision before returning float scene coordinates, so the stock
single-precision Godot scene tree never stores planet-radius coordinates.

Coarse-to-fine updates morph the final two fine-grid rows onto bilinear samples
of the next coarser ring. Exact shared outer/inner boundaries therefore meet
without a T-junction crack while local detail is retained away from the
transition. Distant rings have no collision.

Terrain albedo now also receives a presentation-only rock cue from rendered
slope and elevation. This does not feed back into ecology or geography; it only
prevents coarse living-surface fields from painting steep local cliffs as the
same vegetation color as nearby flats. The walking camera uses a 65-degree field
of view, and the environment keeps lower ambient energy with a stronger warm
directional light so the existing mesh normals produce readable large-scale
form without enabling long-range real-time shadows.

A separate visual-only sea-level surface uses the exact same spherical sample
directions at elevation 0 m. It is opaque in this first slice to stay on the
fast opaque rendering path; terrain above sea level naturally occludes it while
negative-elevation terrain becomes ocean floor.

Meshes are regular indexed `ArrayMesh` surfaces. Collision uses `HeightMapShape3D`, which Godot documents as the terrain-specialized alternative to a concave triangle collision shape:

- https://docs.godotengine.org/en/4.7/classes/class_arraymesh.html
- https://docs.godotengine.org/en/4.7/classes/class_heightmapshape3d.html

Mesh/physics-resource creation remains on the main thread. Godot does not make the active scene tree thread-safe, and its documentation warns that GPU-facing resource work on background threads can stall. The bounded one-chunk-per-frame policy is the simpler first slice:

- https://docs.godotengine.org/en/4.7/tutorials/performance/thread_safe_apis.html

If profiling later shows that walking-speed terrain cannot meet frame-time targets, generation can be split into CPU sampling and main-thread resource upload. That change is not justified before measurement.

## Distant spherical rendering

The distant renderer follows the geometry-clipmap principle of nested regular
grids centered around the viewer, while retaining CPU-generated ArrayMesh data
for this first implementation. GPU Gems describes the established clipmap
structure as nested regular grids with progressively coarser samples and
transition regions between levels:

- https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry-clipmaps

WorldSim deliberately does not move authoritative height generation into a GPU
height texture. The simulation remains authoritative and the Godot adapter
continues to reconstruct presentation heights from active simulation state.
Only the rendering LOD changed.

The Camera3D far plane is 400 km. Non-volumetric environment fog uses Godot
4.7's depth mode from 35 km to 300 km with aerial perspective, so distant
terrain fades into the procedural sky instead of terminating at a local flat
chunk boundary:

- https://docs.godotengine.org/en/4.7/classes/class_camera3d.html
- https://docs.godotengine.org/en/4.7/classes/class_environment.html

Sampling cost and frame time on target player hardware remain **NOT VERIFIED**.
The denser distant grid raises each ring from 1,089 to 4,225 vertices. The
implementation still limits work to one distant LOD rebuild per rendered frame,
recenters every 256 m while walking, uses a much larger threshold during survey
flight, and throttles simulation-driven distant refreshes to avoid coupling
hourly simulation steps to full-horizon mesh regeneration.

## Global map inspection

The Godot host also provides `world_map.tscn`, a separate macro-scale inspection scene. It does not own or regenerate geography. The scene requests an equirectangular elevation array from `WorldSimulationNode`; the adapter converts latitude/longitude pixel centers into directions, resolves their active cube-sphere leaves, and reads `geography.elevation_m`. Each leaf is displayed with its stored value; no smoothing or invented subcell geological detail is applied.

The first viewer uses a fixed 1024 x 512 sampling grid and builds an RGB `Image` / `ImageTexture` at runtime. Elevation-to-color mapping stays in GDScript because it is presentation state, while all terrain heights remain authoritative C++ values. Godot 4.7 documents `Image.create_from_data()`, `ImageTexture.create_from_image()`, and `TextureRect` for this runtime texture path:

- https://docs.godotengine.org/en/4.7/classes/class_image.html
- https://docs.godotengine.org/en/4.7/classes/class_imagetexture.html
- https://docs.godotengine.org/en/4.7/classes/class_texturerect.html

A separate map artifact is also established world-generation practice: WorldEngine keeps generated world data independent from the elevation, precipitation, temperature, biome, and ocean images it emits for inspection. The WorldSim viewer follows that separation without importing WorldEngine's rectangular world model:

- https://github.com/Mindwerks/worldengine

Launch the macro viewer with:

```bash
make map
```

The map deliberately exposes terrain-source defects rather than hiding them. The first global-map pass revealed a radial discontinuity at the antipode of the local azimuthal walking projection. The terrain source is now sphere-native, so that projection is used only to map local walker coordinates to a unit direction and cannot introduce a global terrain singularity. The map remains the visual regression tool for later tectonics and erosion work.

## Tectonic model and debug layers

The kernel contains a deterministic query-only `TectonicModel` that partitions the unit sphere into 16 seeded spherical Voronoi plates. `TerrainGenerator` consumes its continuous macro response, while plate ids, forcing, crust affinity, and raw macro relief remain separately inspectable through debug layers. A plate stores only a seed direction and a normalized relative angular-velocity vector; continental/oceanic crust is **not** a per-plate boolean.

For any unit direction the model selects the owning Voronoi seed, compares its spherical bisectors against every competing seed, and uses the geometrically nearest boundary for the local diagnostic response. It derives:

- owning and neighboring plate ids;
- angular distance to that boundary;
- relative convergence/divergence across the boundary;
- relative shear along the boundary;
- a signed boundary forcing that smoothly decays to zero eight degrees away from the boundary.

Positive forcing represents convergence and negative forcing represents divergence. Angular speeds, convergence, shear, and forcing are normalized relative values, not calibrated SI velocities or geological rates.

The same model also exposes a continuous `continental_affinity` field in `[0,1]`. It is now a low-frequency deterministic 3D value-noise FBM evaluated directly at the normalized unit direction and shaped with a smooth threshold. The field is independent of plate ownership, so one plate may contain both oceanic and continental crust; sampling coherent 3D noise on the sphere keeps it continuous across plate boundaries and the longitude seam without encoding continent silhouettes as unions of radial spherical caps.

Because the lowest-frequency octave spans only a few lattice cells over the whole sphere, its raw spherical mean can drift noticeably between seeds. The model therefore estimates that seed-wide DC component once in the constructor from 128 deterministic equal-area Fibonacci-sphere directions and subtracts it before the affinity threshold. This is a constant bias per world seed: it does not alter local continuity or feature geometry, but prevents otherwise valid seeds from collapsing toward almost entirely oceanic or continental crust.

A core regression samples seeds 0 through 63 on a separate 512-point equal-area sphere. Each seed must retain non-degenerate continental affinity, a transitional crust belt, both uplift and divergence coverage, positive macro terrain plus deep ocean, and plate-boundary continuity. These are broad distribution invariants rather than golden maps.

This follows established sphere-noise practice rather than adding a second flat crust map: libnoise's spherical model samples a 3D noise module on a unit sphere specifically for seamless spherical textures and planetary terrain. WorldSim keeps its own deterministic value-noise implementation and only uses the same sphere-native sampling principle:

- https://libnoise.sourceforge.net/docs/classnoise_1_1model_1_1Sphere.html
- https://libnoise.sourceforge.net/tutorials/tutorial8.html

The tectonic macro height is derived from:

- crust affinity -> broad buoyancy from deep oceanic crust to elevated continental crust;
- convergent boundary response -> positive uplift, stronger on continental crust;
- divergent response -> oceanic ridge uplift or continental rift subsidence;
- transform/shear motion -> no direct vertical term in this slice.

Pair participation still uses the compact smooth competition weight based on how closely both plates approach local ownership; the weight reaches zero with zero slope before the pair is skipped, so the polygon/ghost fix remains intact. Divergence keeps the broad 12-degree macro belt. Convergent uplift is now shaped separately: a continuous sphere-native width field varies its support between roughly 6 and 11.8 degrees, and a second deterministic ridged coherent-noise field modulates the response with a non-zero floor. Both modifiers are multiplied by the actual positive convergence and competition weights, so they can narrow, segment, and branch an orogen but cannot create isolated tectonic mountains away from a convergent boundary. The nearest-boundary forcing diagnostic remains unchanged.

The ridged modulation follows the established procedural-terrain use of absolute-valued coherent noise to create ridge-like mountainous structure, while the geological constraint remains that strong deformation is concentrated near plate boundaries and convergent margins create mountain systems:

- https://libnoise.sourceforge.net/docs/classnoise_1_1module_1_1RidgedMulti.html
- https://libnoise.sourceforge.net/tutorials/tutorial5.html
- https://pubs.usgs.gov/gip/dynamic/understanding.html
- https://volcanoes.usgs.gov/about/edu/dynamicplanet/nutshell.php

The resulting `macro_elevation_m` remains the authoritative low-frequency basis consumed by `TerrainGenerator`; the `Macro relief` map layer continues to show that raw basis without meso/local terrain detail. A seed-42 128 x 64 visual-regression sentinel constrains the convergent uplift footprint so it cannot regress to the previous wide smooth ribbon while the 64-seed robustness sweep still requires non-trivial uplift coverage and boundary continuity.

The global map exposes five inspection layers: authoritative `Elevation`, plus `Plates`, `Tectonic forcing`, `Crust`, and raw `Macro relief`. The latter four remain debug presentations. The `Tectonic forcing` view now renders the terrain-driving response rather than the legacy nearest-pair diagnostic: `uplift_forcing - divergence_forcing` is mapped with a zero-centered red/neutral/blue diverging palette, with a visible neutral gray for inactive interiors. The legacy signed `forcing` array remains exported by the adapter for compatibility and low-level boundary debugging. The `Plates` view adds a presentation-only one-pixel dark topology outline; no outline is overlaid on continuous field layers, so it cannot be mistaken for a terrain discontinuity.

Godot 4.7 documents the existing runtime `Image.create_from_data()` / RGB8 texture path and `Color.lerp()` used by these presentation maps. The signed-response palette follows standard visualization practice for values centered on a meaningful zero:

- https://docs.godotengine.org/en/4.7/classes/class_image.html
- https://docs.godotengine.org/en/4.7/classes/class_color.html
- https://matplotlib.org/stable/users/explain/colors/colormapnorms.html
- https://matplotlib.org/stable/tutorials/colors/colormaps.html

The architectural comparison remains Demiurge's explicit separation between tectonic query state, tectonic debug visualization, and later terrain/erosion consumers. WorldSim uses a much smaller analytical model here rather than copying its baked implementation:

- https://github.com/owenyuwono/demiurge

A spatial bake is intentionally deferred. Plate ownership, crust affinity, and this first macro-relief preview are cheap point queries; erosion and drainage require neighborhood-dependent iterative state and remain the first stage that justifies a persistent cube-sphere bake/cache lifecycle.

## Known boundaries

- Walking terrain follows reconstructed stateful geology. Convergent dry-land belts additionally receive bounded deterministic sub-cell orographic peaks for traversable/rendered mountain relief; this does not change authoritative geography fields or snapshots. Surface color and near decorative vegetation project authoritative climate/hydrology/ecology state through the separate [living-surface contract](LIVING_SURFACE.md).
- The walker has a visual sea-level surface but no river/lake surface geometry, wave simulation, shoreline foam, refraction, or water collision.
- Distant terrain is spherical and extends to roughly 524 km from the viewer (beyond the 400 km camera far plane); it is visual-only and uses progressively coarser samples.
- Terrain revision and synchronized local mesh/collision refresh remain immediate; distant terrain revision refresh is intentionally throttled and continuous fastest-tier survey-flight quality is not verified.
- Frame-time and GPU performance on target player hardware are NOT VERIFIED by CI; CI can verify build, parsing, headless runtime, terrain API, collision scene resources, and core invariants only.

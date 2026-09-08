# Terrain vertical slice

This document records the architecture and performance decisions for the first walkable procedural world slice.

## Scope

The slice intentionally contains only static terrain:

- deterministic planet-scale crust and tectonic macro relief;
- bounded sphere-native meso/local procedural detail over that macro relief;
- no rendered or simulated water;
- no erosion or drainage shaping in the terrain source yet;
- no climate, hydrology, vegetation, fauna, settlements, or other gameplay domains in the Godot terrain scene;
- a first-person walker over streamed terrain chunks.

The existing full default simulation remains available for kernel/domain tests and the diagnostic adapter API.

## Authority and coordinate model

The authoritative spatial model remains the existing cube-sphere hierarchy. No second flat world model is introduced.

`TerrainGenerator` is a deterministic core service keyed by world seed. It owns a `TectonicModel` and can be sampled either from a unit direction on the planet or from local projected meter coordinates. Geography fields are populated from that same generator, so Godot terrain and simulation geography cannot drift because of separate procedural implementations.

The walking projection is azimuthal-equidistant and centered at 45° N, 70° E, but it is not part of terrain generation. `sample_projected()` converts local walker meters to a unit direction and delegates to `sample_direction()`. The authoritative sampler now uses `TectonicModel::macro_elevation_m` as its broad hypsometric base. The previous fixed anisotropic continent cap no longer participates in generation. Existing sphere-native FBM remains only as bounded 180 km rolling relief and 900 m local detail, plus a bounded ridged orogenic term modulated by tectonic uplift and continental affinity. Final `land_fraction` is derived from final elevation across a narrow sea-level transition.

A regression samples 1,024 sphere directions and requires authoritative elevation to correlate above 0.95 with tectonic macro relief while retaining measurable detail and keeping the detail residual below 1.5 km. A separate level-5 cube-sphere integration keeps seed-42 global land coverage in a broad non-degenerate range and preserves dry land at the existing local-walker origin.

This follows the same relevant large-planet practice as Demiurge: terrain is a deterministic function of seed plus spherical position, and its macro pipeline is angular/normalized rather than derived from a global flat map. WorldSim uses its own analytical tectonic model and does not copy Demiurge's implementation:

- https://github.com/owenyuwono/demiurge

Static geography is re-sampled after actual simulation-cover refinement/coarsening through the module lifecycle. This is required because generic intensive-field refinement copies parent values and therefore cannot create higher-frequency terrain detail by itself.

Because authoritative geography semantics are part of persistent world state, snapshot compatibility advances whenever that terrain/geology contract changes. Tectonic authority introduced version 3, orogenic shaping version 4, plate-layout diversification version 5, the minimum-separation correction version 6, stateful geological evolution version 7, burial-dependent sediment compaction version 8, separated fluvial/hillslope geomorphology version 9, depth-dependent regolith production version 10, critical-slope hillslope acceleration version 11, and coast-to-basin marine sediment routing version 12. Global snapshot version 13 added persistent living-soil ecology fields; version 14 added persistent grass/shrub/tree functional-type pools and propagule-limited vegetation semantics; version 15 adds habitat-selected fauna redistribution without changing the geology generator itself. Versions 2 through 14 are rejected by the current authoritative-world snapshot contract.

## Godot large-world strategy

The stock Godot project remains a normal single-precision build. Logical projected coordinates are kept as GDScript scalar values while scene-tree coordinates are periodically shifted back near the origin. Terrain chunk transforms are rebuilt relative to that logical origin.

This follows Godot's documented precision limits for large single-precision worlds and uses origin shifting instead of requiring a custom double-precision engine build:

- https://docs.godotengine.org/en/4.7/tutorials/physics/large_world_coordinates.html

The initial shift threshold is 1,024 m, comfortably inside the range where Godot documents high positional precision for first-person gameplay.

## Terrain streaming

The first implementation deliberately avoids a general clipmap/CDLOD system.

Current chunk contract:

- chunk size: 256 m;
- mesh resolution: 33 x 33 vertices;
- vertex spacing: 8 m;
- visible radius: 3 chunks;
- retained radius: 4 chunks;
- at most one missing chunk is generated per rendered frame;
- the chunk under the initial player position is built synchronously before movement starts.

Meshes are regular indexed `ArrayMesh` surfaces. Collision uses `HeightMapShape3D`, which Godot documents as the terrain-specialized alternative to a concave triangle collision shape:

- https://docs.godotengine.org/en/4.7/classes/class_arraymesh.html
- https://docs.godotengine.org/en/4.7/classes/class_heightmapshape3d.html

Mesh/physics-resource creation remains on the main thread. Godot does not make the active scene tree thread-safe, and its documentation warns that GPU-facing resource work on background threads can stall. The bounded one-chunk-per-frame policy is the simpler first slice:

- https://docs.godotengine.org/en/4.7/tutorials/performance/thread_safe_apis.html

If profiling later shows that walking-speed terrain cannot meet frame-time targets, generation can be split into CPU sampling and main-thread resource upload. That change is not justified before measurement.

## Why not geometry clipmaps yet?

Geometry clipmaps are a strong fit for very large view distances, flight, and a nearly constant terrain rendering budget. The established GPU Gems implementation uses nested regular grids centered on the viewer and continuously shifts/refills them:

- https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry-clipmaps

That solves a different immediate problem than this slice. At walking speed, fixed local chunks make collision, origin shifting, correctness, and simulation/geography agreement testable with substantially less machinery. A clipmap or another terrain render-LOD scheme should be introduced when distant-horizon requirements or profiling make the trade-off concrete.

## Global map inspection

The Godot host also provides `world_map.tscn`, a separate macro-scale inspection scene. It does not own or regenerate geography. The scene requests an equirectangular elevation array from `WorldSimulationNode`; the adapter samples the same core `TerrainGenerator::sample_direction()` used by simulation geography and converts only latitude/longitude pixel centers into unit directions.

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

- Terrain elevation now uses tectonic macro relief as its authoritative broad basis, but it has no erosion, sediment transport, or drainage shaping yet.
- "Ocean" currently means generated ocean floor/land mask. No water surface exists in this slice.
- There is no distant terrain LOD or planetary horizon in the local walker; the global map is a separate 2D inspection tool.
- Terrain is currently immutable except through changing the world seed/source implementation.
- Frame-time and GPU performance on target player hardware are NOT VERIFIED by CI; CI can verify build, parsing, headless runtime, terrain API, collision scene resources, and core invariants only.

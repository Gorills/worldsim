# Terrain vertical slice

This document records the architecture and performance decisions for the first walkable procedural world slice.

## Scope

The slice intentionally contains only static terrain:

- one deterministic continent at approximately Eurasia scale;
- ocean floor everywhere outside that continent;
- no rendered or simulated water;
- no climate, hydrology, vegetation, fauna, settlements, or other gameplay domains in the Godot terrain scene;
- a first-person walker over streamed terrain chunks.

The existing full default simulation remains available for kernel/domain tests and the diagnostic adapter API.

## Authority and coordinate model

The authoritative spatial model remains the existing cube-sphere hierarchy. No second flat world model is introduced.

`TerrainGenerator` is a stateless deterministic core service keyed by world seed. It can be sampled either from a unit direction on the planet or from local projected meter coordinates. Geography fields are populated from that same generator, so Godot terrain and simulation geography cannot drift because of separate procedural implementations.

The walking projection is azimuthal-equidistant and centered at 45° N, 70° E, but it is no longer part of the authoritative terrain-generation path. `sample_projected()` converts local walker meters to a unit direction and delegates to `sample_direction()`. The sphere-native sampler evaluates deterministic 3D value-noise FBM directly from the unit direction and uses a smooth anisotropic spherical cap for the initial continent. This removes the azimuthal antipode singularity from generated geography while preserving one approximately Eurasia-scale landmass with nominal tangent semi-axes of 6,500 km by 2,700 km. A regression test still integrates the level-5 cube-sphere cover and requires generated land area between 40 and 70 million km².

This follows the same relevant large-planet practice as Demiurge: terrain is a deterministic function of seed plus spherical position, and its macro pipeline is angular/normalized rather than derived from a global flat map. WorldSim does not copy Demiurge's implementation or tectonics in this change; it uses the principle only to keep one authoritative sphere-native sampling path:

- https://github.com/owenyuwono/demiurge

Static geography is re-sampled after actual simulation-cover refinement/coarsening through the module lifecycle. This is required because generic intensive-field refinement copies parent values and therefore cannot create higher-frequency terrain detail by itself.

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

## Tectonic debug model

The kernel now contains a deterministic query-only `TectonicModel` that is independent from `TerrainGenerator`. It partitions the unit sphere into 16 seeded spherical Voronoi plates. A plate stores a seed direction, a continental/oceanic crust flag, and an angular-velocity vector. For any unit direction the model selects the two nearest plate seeds, treats their spherical bisector as the local plate boundary, and derives:

- owning and neighboring plate ids;
- angular distance to that boundary;
- relative convergence/divergence across the boundary;
- relative shear along the boundary;
- a signed boundary forcing that smoothly decays to zero eight degrees away from the boundary.

Positive forcing represents convergence and negative forcing represents divergence. The values are diagnostic kinematics only in this slice: they do **not** modify `geography.elevation_m` or the local terrain mesh yet. This separation is deliberate so plate ownership and boundary motion can be validated before they become an input to macro relief.

The global map samples the tectonic model through the Godot adapter and exposes three presentation-only layers: `Elevation`, `Plates`, and `Tectonic forcing`. Plate colors and red/blue forcing colors live only in GDScript. Godot 4.7 documents the standard `Button.pressed` signal used by the layer controls and `PackedInt32Array` used for plate ids:

- https://docs.godotengine.org/en/4.7/classes/class_button.html
- https://docs.godotengine.org/en/4.7/classes/class_packedint32array.html

The architectural comparison remains Demiurge's explicit separation between tectonic query state, tectonic debug visualization, and later terrain/erosion consumers. WorldSim uses a much smaller analytical model here rather than copying its baked implementation:

- https://github.com/owenyuwono/demiurge

A spatial bake is intentionally deferred. Ownership and boundary kinematics are cheap point queries; erosion and drainage will require neighborhood-dependent iterative state and are the first stage that justifies a persistent cube-sphere bake/cache lifecycle.

## Known boundaries

- Terrain elevation is still procedural and is not yet driven by the tectonic model.
- "Ocean" currently means generated ocean floor/land mask. No water surface exists in this slice.
- There is no distant terrain LOD or planetary horizon in the local walker; the global map is a separate 2D inspection tool.
- Terrain is currently immutable except through changing the world seed/source implementation.
- Frame-time and GPU performance on target player hardware are NOT VERIFIED by CI; CI can verify build, parsing, headless runtime, terrain API, collision scene resources, and core invariants only.

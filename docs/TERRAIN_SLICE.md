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

The walking projection is azimuthal-equidistant and centered at 45° N, 70° E. The initial continent uses nominal semi-axes of 6,500 km by 2,700 km plus deterministic coastline perturbation. A regression test integrates the level-5 cube-sphere cover and requires generated land area to remain between 40 and 70 million km²; the current seed is approximately Eurasia-scale, not an attempt to reproduce real Eurasian coastlines.

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

## Known boundaries

- The procedural function is visual/gameplay terrain, not a geological or tectonic simulation.
- "Ocean" currently means generated ocean floor/land mask. No water surface exists in this slice.
- There is no distant terrain LOD or planetary horizon yet.
- Terrain is currently immutable except through changing the world seed/source implementation.
- Frame-time and GPU performance on target player hardware are NOT VERIFIED by CI; CI can verify build, parsing, headless runtime, terrain API, collision scene resources, and core invariants only.

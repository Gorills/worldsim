# Terrain rendering contract

This document describes the current walkable terrain presentation contract. It is not a product roadmap and it does not define natural-world development priority.

## Authority

The authoritative spatial model is the simulation's cube-sphere hierarchy and adaptive cover. Persistent geology owns macro elevation and related state.

Godot terrain rendering may reconstruct and decorate that state for visual/physics resolution, but presentation detail never becomes a second authoritative terrain model and is never fed back into geology, hydrology or ecology.

Rendering LOD and simulation LOD remain separate.

## Walking-surface reconstruction

Simulation cells are much larger than walking-mesh vertices. Rendering the active-cell value directly would create broad terraces, while refining simulation state to render resolution would make presentation requirements own the world-simulation budget.

The adapter therefore reconstructs a smooth macro surface from authoritative `geography.elevation_m` and adds only a deterministic high-frequency residual from the seed terrain generator:

```text
render_height(p)
  = reconstruct(authoritative elevation, p)
  + seed_detail(p)
  - reconstruct(seed_detail anchors, p)
```

The reconstruction resolves the actual active cover before combining values, so a coarse ancestor or active descendants are handled through the existing adaptive-world semantics. The procedural residual is presentation-only; stored field values, erosion routing, mass budgets and snapshots remain unchanged.

Terrain/collision updates use the same sampled height array whenever authoritative terrain changes.

## Local walking terrain

The current near terrain uses streamed regular heightfield chunks:

- 256 m chunk size;
- 33 x 33 mesh/collision samples per chunk;
- 8 m central vertex spacing;
- `ArrayMesh` for rendering;
- `HeightMapShape3D` for walking collision;
- a bounded visible/retained chunk neighborhood around the player;
- missing/dirty chunks generated incrementally on the main thread.

The player chunk is available synchronously when required for safe movement. Scene-tree/GPU resource creation remains on the Godot main thread; there is no speculative background scene mutation path.

## Distant terrain

Distant presentation uses nested regular terrain rings centered on the logical player position. Sample spacing increases with distance. Distant rings have no gameplay collision.

Near and distant surfaces sample the same terrain adapter. Coarse/fine ring boundaries morph toward the coarser grid so presentation LOD changes do not create visible cracks.

Distant rendering is a presentation optimization only. It does not alter simulation focus, active-cover authority or persistence.

## Large-world coordinates

The stock single-precision Godot build does not store planet-radius scene coordinates directly.

WorldSim keeps logical projected coordinates separately and periodically rebases scene-tree positions near the origin. Terrain transforms, player movement and map conversions use the logical position as the stable reference.

Simulation coordinates remain sphere-native; the local walking projection is an engine/client concern.

## Living-surface projection

Terrain geometry and living-surface appearance have separate revision paths.

Authoritative ecology/climate state such as vegetation, snow, flooding and fire may change terrain color/decorative vegetation without forcing height/collision regeneration. Near decorative trees/shrubs are visual projections of authoritative biomass and never own ecological stocks.

See `LIVING_SURFACE.md` for the surface-presentation contract.

## Godot boundary

The renderer consumes authoritative terrain/surface data through the existing GDExtension/native adapter boundary. Godot may cache meshes, colors and reconstruction samples, but those caches are disposable presentation state.

A future resource-interaction system must not infer authoritative gatherable material from decorative meshes. `Resource Acquisition v1` must query/command the simulation-owned resource state defined by `DIRECTION.md`.

## Validation

Terrain changes should preserve the executable behaviors already covered by the Godot integration tests, including:

- authoritative elevation changes affect reconstructed walking height;
- adjacent terrain patches share compatible boundaries;
- render mesh and collision use matching height samples;
- adaptive-cover changes invalidate/rebuild the required terrain presentation;
- survey/fast-travel maintains large logical coordinates and safely restores walking;
- the world map reads the authoritative macro terrain source rather than a separate presentation generator;
- the walking scene renders successfully in CI visual capture.

Performance on final target hardware is not implied by CI. Optimize chunk generation or GPU upload only after profiling identifies a concrete frame-time/streaming bottleneck.

## Non-goals

This contract does not make terrain rendering authoritative, does not require simulation LOD to match render LOD, does not claim production graphics quality, and does not define gameplay resources, mining, buildings or settlements.
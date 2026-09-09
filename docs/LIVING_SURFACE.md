# Living surface v1 — design and rendering contract

## Decision

The walking viewer now consumes the existing full authoritative simulation instead
of constructing a geography-only terrain simulation. Surface appearance is a
projection of already registered climate, hydrology and ecology state; Godot
does not own a second biome, vegetation, snow, flood or fire model.

The bounded v1 presentation exposes:

- grass, shrub and tree carbon as land-normalized kgC/m2 densities;
- authoritative snow-cover fraction;
- hydrology flooded fraction;
- current fire-active and fire-burned fractions.

The adapter returns these quantities in sample-aligned arrays through
`sample_surface_visual_patch()`. It resolves the active simulation cell at
each requested spherical point and converts extensive PFT carbon to density
using the authoritative cell area and land fraction. Rendering code only maps
these physical/reduced quantities to colors and deterministic decorative
instances.

There is deliberately no discrete biome taxonomy in this slice. A renderer
classification such as forest/desert/tundra would duplicate state already
represented by climate, water, PFT biomass and snow and could diverge from the
simulation.

## Near and distant presentation

Near walking chunks retain the existing 8 m terrain/collision contract.
Their vertex colors now use the surface packet rather than elevation alone.
Grass remains material-only in v1. Shrub and tree biomass additionally controls
visual-only `MultiMeshInstance3D` instances inside each local chunk.

Instance positions are deterministic from the stable world chunk coordinate and
candidate index. Rebuilding a chunk, shifting the floating origin or changing
rendering LOD therefore does not create simulation objects or feed placement
state back into the kernel. The instances are disposable presentation.

Distant spherical terrain uses the same surface packet and the same color
mapping, but does not create vegetation geometry. This keeps the long-range
horizon budget independent from local decorative density.

Godot 4.7 documents `MultiMeshInstance3D` as the specialized node for many
instances of one mesh, and its performance guide recommends splitting
MultiMeshes spatially because culling applies to the whole MultiMesh rather than
individual instances:

- https://docs.godotengine.org/en/4.7/classes/class_multimeshinstance3d.html
- https://docs.godotengine.org/en/4.7/classes/class_multimesh.html
- https://docs.godotengine.org/en/4.7/tutorials/performance/using_multimesh.html

The current implementation keeps one tree and one shrub MultiMesh per retained
near chunk, so the existing terrain streaming boundary also bounds vegetation
visibility/culling granularity.

## Simulation LOD contract

Surface point queries target the configured maximum simulation-level region,
then resolve the active cover exactly as the existing diagnostic point-query
path does. Intensive fields are read from that active cell. Extensive plant
carbon is divided by the active cell's effective land area.

This is a presentation projection, not simulation prolongation. No field value,
cell cover, conserved stock or snapshot representation is changed.

The important refine/coarsen invariant is that a pure refinement of an
unchanged cell preserves plant density: extensive carbon splits by cell area,
while land fraction remains intensive. Coarsening recovers the corresponding
area-weighted authoritative state. The renderer never uses topology
`neighbors4()` as if it were an active-cover query.

This follows the repository's existing AMR separation: simulation state
restriction/prolongation remains authoritative, while a consumer reconstructs
what it needs after resolving the active cover. Comparable AMR practice keeps
coarse/fine state transfer separate from visualization:

- https://amrex-codes.github.io/amrex/docs_html/AmrCore.html
- https://doi.org/10.1016/0021-9991(89)90035-1

## Refresh contract

Terrain and surface revisions are separate transient adapter counters.

`terrain_revision` continues to track authoritative elevation changes.
`surface_revision` fingerprints only the fields used by living-surface
presentation. The near viewer observes every new surface revision but applies
chunk refreshes at a bounded presentation interval; distant terrain keeps its
existing multi-second refresh throttle. Physics collision is still derived only
from terrain heights.

Neither revision is serialized or authoritative world state.

## Explicit boundaries

Living surface v1 does not add:

- biome classes;
- species-specific vegetation assets;
- grass blade geometry;
- river or lake surface meshes;
- water refraction, waves or shore foam;
- weather particles or cloud simulation;
- fauna embodiment;
- renderer-owned ecological state.

Near vegetation remains deliberately low-detail presentation, but trees now use
a separate low-poly trunk and rounded canopy surface and shrubs use a rounded
low-poly crown instead of the diagnostic cone silhouettes. Final art quality and
target-hardware frame time remain **NOT VERIFIED**. CI can verify the surface
packet, revision propagation, Godot parsing/headless runtime, near MultiMesh
creation and primitive structure, distant color arrays and existing
collision/terrain invariants.

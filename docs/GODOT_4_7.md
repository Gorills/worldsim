# Godot 4.7 integration

The Godot layer is an adapter, not the simulation owner.

## Versions

- Godot target API: **4.7**
- Current tested-target maintenance release at archive finalization: **4.7.2**
- `godot-cpp`: **10.0.0-rc2**

`godot-cpp` 10.x supports explicit API targeting; rc2 updated its CMake API version support to 4.7. The CMake configuration sets `GODOTCPP_API_VERSION=4.7`.

References:

- <https://docs.godotengine.org/en/4.7/tutorials/scripting/cpp/index.html>
- <https://docs.godotengine.org/en/4.7/engine_details/engine_api/gdextension/gdextension_file.html>
- <https://github.com/godotengine/godot-cpp/releases/tag/10.0.0-rc2>
- <https://godotengine.org/download/archive/4.7.2-stable/>

## Build

```bash
cmake --preset godot-dev
cmake --build --preset godot-dev
ctest --preset godot-dev
```

To launch the viewer scene locally:

```bash
make run
```

`make run` builds the `worldsim_godot` target, then runs `scripts/run_godot.sh`, which starts Godot 4.7.x with `godot/project/main.tscn`. Set `GODOT` if the editor is not on `PATH`.

CMake writes the extension library directly into `godot/project/bin/`. The `.gdextension` resource points at that directory.

If network fetches are forbidden in your build environment, place the exact godot-cpp source tree at:

```text
third_party/godot-cpp/
```

and configure with `-DWORLDSIM_FETCH_GODOT_CPP=OFF`.

## Exposed node

`WorldSimulationNode` exposes:

- `initialize(seed)` for the full default simulation
- `initialize_terrain_world(seed)` for the terrain-only walking slice
- `step_hours(hours)`
- `set_focus_direction(direction)` / `set_focus_projected(east_m, north_m)` / `clear_focus()`
- `projected_to_direction(east_m, north_m)` for presentation-space placement on sphere maps
- `sample_terrain_height(east_m, north_m)` for reconstructed authoritative walking height
- `sample_terrain_patch(center_east_m, center_north_m, spacing_m, resolution)` for reconstructed authoritative walking chunks
- `sample_terrain_equirectangular(width, height)` for current authoritative elevation at active-cell resolution
- `sample_field_equirectangular(field_key, width, height, normalize_extensive)` for generic live scalar maps
- `sample_lod_equirectangular(width, height)` for the active simulation-level map
- `inspect_direction(direction)` for exact active-cell metadata and raw registered fields
- `sample_preview_terrain_equirectangular(width, height)` for the immutable seed-terrain diagnostic
- `sample_tectonics_equirectangular(width, height)`
- `get_tick()`
- `get_terrain_revision()` for presentation cache invalidation
- `get_render_packet()`
- `get_field_descriptors()`
- `get_field_values(field_key)`
- `drain_events()`
- `schedule_field_impulse(cell_id_hi, cell_id_lo, field_key, delta)`
- `get_last_error()`

All public adapter methods catch C++ exceptions before returning to Godot. Errors are reported with `UtilityFunctions::push_error()` and retained in `get_last_error()`.

Walking height and patch sampling reconstruct the current authoritative
`geography.elevation_m` field and add only the sub-cell residual of the static
procedural terrain. The resulting point surface is presentation data: the
simulation cell values remain authoritative, while the procedural residual must
not be fed back into geology, hydrology, or persistence. The separately named
`sample_preview_terrain_equirectangular()` API remains the immutable seed preview.

For global-map diagnostics, `sample_tectonics_equirectangular()` returns aligned `plate_id`, legacy signed `forcing`, terrain-driving `uplift_forcing` and `divergence_forcing`, `crust_affinity`, and `macro_elevation_m` arrays. These arrays are debug adapter data only; they do not become authoritative Godot-owned state.

## 64-bit cell IDs

`CellId` is unsigned 64-bit. GDScript integers are signed 64-bit, so the adapter **does not** reinterpret a raw `uint64_t` as a GDScript integer. It exposes `cell_id_hi` and `cell_id_lo`, each in range `0..2^32-1`. Recombine only inside an unsigned-capable environment, or pass the two halves back to the adapter.

## Render packet

Arrays are aligned by index and describe the current active LOD cover:

```text
positions
areas_m2
levels
cell_id_hi
cell_id_lo
temperature_c            demo convenience field
vegetation_kg_m2         demo convenience density
mana_j_m2                demo convenience density
```

For new domains, use `get_field_descriptors()` and `get_field_values(key)` instead of adding new hard-coded getters.

## Full-world simulation laboratory

`simulation_lab.tscn` initializes `make_default_simulation()` through
`WorldSimulationNode.initialize()`. Run it with `make lab`. It is deliberately
separate from the terrain walker and the immutable tectonic viewer.

The laboratory provides play/pause and 1-hour/1-day/30-day steps, a generic
field selector, exact active-cover equirectangular maps, simulation-LOD coloring,
an explicit color legend, active-cell min/area-mean/max and p02/p98 statistics,
authoritative totals for extensive fields, selected-field history, exact-cell
inspection and explicit focus controls. Extensive fields are colored as value
per represented square metre while the inspector retains raw stored values.

Each field retains a grow-only percentile color range during a run so a changing
timestep cannot create a false visual trend by silently shrinking the scale.
`Reset range` is the explicit rescale operation. See `SIMULATION_LAB.md` for the
data, interaction, reference and performance contracts.

## Viewer baseline

The main scene is now a first-person terrain walker backed by the terrain-only simulation factory. It streams regular 256 m terrain chunks around the player, uses `ArrayMesh` for rendering and `HeightMapShape3D` for collision, and bounds foreground work to one missing or dirty chunk per rendered frame after the initial player chunk. The adapter fingerprints the active cell ids and authoritative elevation values around each simulation step; a changed fingerprint advances `get_terrain_revision()`. The chunk under the player is then rebuilt synchronously so visible geometry and collision cannot disagree there, while the rest of the visible set is refreshed under the normal per-frame budget. Chunk triangles use Godot 4.7 clockwise winding so the ground faces +Y; the editor preview environment is not used at runtime, so the scene includes a `WorldEnvironment` with `ProceduralSkyMaterial`.

Logical projected world coordinates remain in 64-bit GDScript scalar values while scene nodes are origin-shifted at a 1,024 m threshold. The stock single-precision Godot build therefore does not need to place scene nodes millions of meters from the origin.

The viewer defines InputMap actions for WASD movement and jump. Mouse look is handled through `_unhandled_input()`; Escape releases/captures the mouse.

`F` switches between grounded walking and non-colliding survey flight. In flight,
WASD follows the camera, Space/Q move vertically, the mouse wheel selects 0.25,
2.5, 25, or 250 km/s, and Shift temporarily multiplies the selected speed by
four. Entering flight raises the camera to at least 250 m above the local ground.
Returning to walking synchronously creates the destination chunk and places the
character 1.25 m above its sampled surface before restoring grounded collision.
This is presentation/navigation state only and does not transfer authority from
the simulation kernel.

Fast flight integrates horizontal motion directly into the 64-bit logical origin,
so even a maximum-speed physics step never places a multi-million-meter position
in the single-precision scene tree. It otherwise reuses the established
origin-shifting contract.
Only the destination tile is forced synchronously when a physics step crosses
chunks; surrounding tiles retain the one-per-rendered-frame budget. Consequently,
terrain around the camera can visibly fill in after the highest-speed travel.
This is intentionally a point-to-point inspection control, not distant terrain
LOD. Geometry clipmaps remain the documented next step if continuous high-speed
horizon rendering becomes a requirement.

The input and motion choices were checked against the Godot 4.7 `Input` and
`CharacterBody3D` contracts. Variable speed follows established debug-camera
practice represented by Unreal Engine's `ADebugCameraController::SpeedScale`:

- https://docs.godotengine.org/en/4.7/classes/class_input.html
- https://docs.godotengine.org/en/4.7/tutorials/inputs/input_examples.html
- https://docs.godotengine.org/en/4.7/classes/class_characterbody3d.html
- https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/Engine/ADebugCameraController/SpeedScale?application_version=5.5

HUD text is localized through gettext PO catalogs (English and Russian) and styled by a shared `Theme` resource.

### Walking world map

The walking HUD includes a north-up, whole-sphere equirectangular map in the top
right. Its 320 x 160 base texture is generated once from the initial
authoritative `geography.elevation_m` cover. It deliberately omits procedural
sub-cell presentation detail, which is not legible at whole-planet scale. Color
elevation bands, restrained relief shading, and a coastline accent preserve
legibility at the small display size. Latitude/longitude guides are a separate
overlay. The base texture is not regenerated after geological revisions; the
dedicated global-map scene remains the live exact-cell diagnostic.

The live marker layer converts the walker's 64-bit projected coordinates through
`TerrainGenerator::projected_to_direction()`, then maps that unit direction to
equirectangular UV. A second projected point 50 km ahead supplies the arrow
heading; the horizontal delta wraps across the longitude seam. The overlay is
redrawn without rebuilding the terrain texture. This follows the usual minimap
separation between a static base map and live position/orientation markers while
keeping engine/UI state non-authoritative.

Godot 4.7 custom drawing and `Control.queue_redraw()` are the engine contract for
the marker overlay. A published survey of game minimaps identifies map
orientation, player/world centering, corner placement, and directional cues as
the relevant design dimensions; this viewer deliberately uses a world-centered,
north-up overview with a heading arrow in the requested top-right corner:

- https://docs.godotengine.org/en/4.7/tutorials/2d/custom_drawing_in_2d.html
- https://docs.godotengine.org/en/4.7/classes/class_control.html
- https://www.mdpi.com/2220-9964/12/2/58

The scene is intentionally terrain-only. There is no rendered water, atmosphere, vegetation, fauna, content/entity presentation, or distant terrain render LOD. See `docs/TERRAIN_SLICE.md` for the architecture and scaling decision.

## CI build strategy

The native adapter only derives from `godot::Node`; Godot rendering classes are instantiated from GDScript. `godot/build_profile.json` therefore enables `Node` plus `OS`: `OS` is not used by WorldSim directly, but is required by the handwritten `godot-cpp` core source `src/core/print_string.cpp`. The profile still avoids generation/compilation of unrelated engine classes and materially reduces cold CI cost.

The current workflow builds only the `worldsim_godot` target in the Godot job. It relies on the feature build profile and does not configure a compiler-cache service.

The integration job then downloads/caches the exact Godot 4.7.2 Linux editor and verifies its SHA-256. A standalone headless editor `--import` pass is best-effort because `godot-cpp` upstream CI documents that this editor-import process can abort after generating `.godot`. The adapter gate is `res://ci_smoke.gd`, which loads the extension at runtime, creates `WorldSimulationNode`, advances the simulation, and checks returned data. Runtime scene gates additionally cover the terrain walker, survey flight, global tectonic map and `res://ci_simulation_lab.gd` full-world diagnostic workflow.

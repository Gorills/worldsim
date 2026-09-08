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
- `sample_terrain_height(east_m, north_m)`
- `sample_terrain_patch(center_east_m, center_north_m, spacing_m, resolution)`
- `sample_terrain_equirectangular(width, height)`
- `sample_tectonics_equirectangular(width, height)`
- `get_tick()`
- `get_render_packet()`
- `get_field_descriptors()`
- `get_field_values(field_key)`
- `drain_events()`
- `schedule_field_impulse(cell_id_hi, cell_id_lo, field_key, delta)`
- `get_last_error()`

All public adapter methods catch C++ exceptions before returning to Godot. Errors are reported with `UtilityFunctions::push_error()` and retained in `get_last_error()`.

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

## Viewer baseline

The main scene is now a first-person terrain walker backed by the terrain-only simulation factory. It streams regular 256 m terrain chunks around the player, uses `ArrayMesh` for rendering and `HeightMapShape3D` for collision, and bounds foreground work to one missing chunk per rendered frame after the initial player chunk. Chunk triangles use Godot 4.7 clockwise winding so the ground faces +Y; the editor preview environment is not used at runtime, so the scene includes a `WorldEnvironment` with `ProceduralSkyMaterial`.

Logical projected world coordinates remain in 64-bit GDScript scalar values while scene nodes are origin-shifted at a 1,024 m threshold. The stock single-precision Godot build therefore does not need to place scene nodes millions of meters from the origin.

The viewer defines InputMap actions for WASD movement and jump. Mouse look is handled through `_unhandled_input()`; Escape releases/captures the mouse.

HUD text is localized through gettext PO catalogs (English and Russian) and styled by a shared `Theme` resource.

The scene is intentionally terrain-only. There is no rendered water, atmosphere, vegetation, fauna, content/entity presentation, or distant terrain render LOD. See `docs/TERRAIN_SLICE.md` for the architecture and scaling decision.

## CI build strategy

The native adapter only derives from `godot::Node`; Godot rendering classes are instantiated from GDScript. `godot/build_profile.json` therefore enables `Node` plus `OS`: `OS` is not used by WorldSim directly, but is required by the handwritten `godot-cpp` core source `src/core/print_string.cpp`. The profile still avoids generation/compilation of unrelated engine classes and materially reduces cold CI cost.

For repeat builds, GitHub Actions uses `mozilla-actions/sccache-action@v0.0.11` with `SCCACHE_GHA_ENABLED=true` and `CMAKE_CXX_COMPILER_LAUNCHER=sccache`, matching the mechanism used by the upstream `godot-cpp` 10.0.0-rc2 CMake CI. The workflow builds only the `worldsim_godot` target rather than rebuilding tests and CLI in the Godot job.

The integration job then downloads/caches the exact Godot 4.7.2 Linux editor and verifies its SHA-256. A standalone headless editor `--import` pass is best-effort because `godot-cpp` upstream CI documents that this editor-import process can abort after generating `.godot`. The mandatory gate is `res://ci_smoke.gd`, which loads the extension at runtime, creates `WorldSimulationNode`, advances the simulation, and checks returned data.

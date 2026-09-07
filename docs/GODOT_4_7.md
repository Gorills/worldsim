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

CMake writes the extension library directly into `godot/project/bin/`. The `.gdextension` resource points at that directory.

If network fetches are forbidden in your build environment, place the exact godot-cpp source tree at:

```text
third_party/godot-cpp/
```

and configure with `-DWORLDSIM_FETCH_GODOT_CPP=OFF`.

## Exposed node

`WorldSimulationNode` exposes:

- `initialize(seed)`
- `step_hours(hours)`
- `set_focus_direction(direction)` / `clear_focus()`
- `get_tick()`
- `get_render_packet()`
- `get_field_descriptors()`
- `get_field_values(field_key)`
- `drain_events()`
- `schedule_field_impulse(cell_id_hi, cell_id_lo, field_key, delta)`
- `get_last_error()`

All public adapter methods catch C++ exceptions before returning to Godot. Errors are reported with `UtilityFunctions::push_error()` and retained in `get_last_error()`.

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

## MultiMesh demo

The included scene builds a `MultiMesh` of cells and colors it using temperature, vegetation and mana. The material explicitly enables `vertex_color_use_as_albedo`; Godot requires that flag for per-instance MultiMesh colors to affect `StandardMaterial3D`.

The demo is intentionally a visualization host. Terrain meshing, atmosphere, oceans, entity rendering, camera controls, UI, and gameplay should be written as Godot-side consumers of simulation data rather than moved into the kernel.

## CI build strategy

The native adapter only derives from `godot::Node`; Godot rendering classes are instantiated from GDScript. `godot/build_profile.json` therefore enables `Node` plus `OS`: `OS` is not used by WorldSim directly, but is required by the handwritten `godot-cpp` core source `src/core/print_string.cpp`. The profile still avoids generation/compilation of unrelated engine classes and materially reduces cold CI cost.

For repeat builds, GitHub Actions uses `mozilla-actions/sccache-action@v0.0.11` with `SCCACHE_GHA_ENABLED=true` and `CMAKE_CXX_COMPILER_LAUNCHER=sccache`, matching the mechanism used by the upstream `godot-cpp` 10.0.0-rc2 CMake CI. The workflow builds only the `worldsim_godot` target rather than rebuilding tests and CLI in the Godot job.

The integration job then downloads/caches the exact Godot 4.7.2 Linux editor and verifies its SHA-256. A standalone headless editor `--import` pass is best-effort because `godot-cpp` upstream CI documents that this editor-import process can abort after generating `.godot`. The mandatory gate is `res://ci_smoke.gd`, which loads the extension at runtime, creates `WorldSimulationNode`, advances the simulation, and checks returned data.

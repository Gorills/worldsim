# WorldSim

WorldSim is a headless C++20 world-simulation kernel designed for a persistent, planet-scale game world whose simulation fidelity changes with spatial LOD without changing the authoritative world model. The included vertical slice covers geography, climate/weather, basin hydrology with snow, shallow groundwater, delayed river routing and lake storage, living-soil fertility/detritus, grass/shrub/tree plant functional types with local dispersal and competition, cohort fauna with local habitat-selected migration, and an independent magic domain. It also includes a stable C ABI and a Godot 4.7 GDExtension host project.

This repository is **not** claiming that the included climate/ecology equations are a scientifically calibrated Earth model. They are deliberately replaceable domain implementations used to exercise the production architecture: spatial hierarchy, conservation across LOD, scheduling, state stores, snapshots, commands, events, engine boundaries, and module extension.

## Design invariants

1. **The simulation owns the world; the renderer does not.** Godot, Unreal, a custom renderer, a server, or tools consume snapshots/field arrays through an engine boundary.
2. **Simulation LOD is state aggregation/refinement, not object sleeping.** Far-away state remains authoritative at coarser resolution.
3. **LOD transitions preserve conserved quantities.** Extensive fields (water, biomass, mana, resources, money-like stocks) split/sum; intensive fields (temperature, ratios, prices where appropriate) use semantic aggregation rules.
4. **Domains are modules.** A module registers fields, state stores, and systems. The core does not contain special cases for magic, ecology, disease, economy, politics, etc.
5. **Systems declare data access and ordering.** The scheduler rejects unordered read/write conflicts instead of silently depending on registration order.
6. **Time is fixed-step and deterministic within a build/toolchain for equal seed + inputs.** Randomness is stateless and derived from seed/stream/tick/object identifiers.
7. **Snapshots are continuation state.** LOD cover, fields, cohorts, queued commands, pending events, focus, and store versions are serialized in a versioned little-endian format.

## Repository layout

```text
include/worldsim/      Public C++ and C ABI
src/                   Simulation kernel and default domain modules
tests/                 Conservation, determinism, LOD, snapshot and ABI tests
apps/                   Headless CLI smoke application
godot/                  Godot 4.7 GDExtension adapter + visualization host
docs/                   Architecture and extension documentation
scripts/                Reproducible build entry points
```

## Build core

Requirements: CMake 3.24+, Ninja, a C++20 compiler.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./out/dev/worldsim_cli
```

Or run `scripts/build_core.sh` / `scripts/build_core.ps1`.

## Inspect basin hydrology

```bash
./out/dev/worldsim_hydrology_dump --days 730 --level 2 --adaptive --output out/hydrology
python3 scripts/plot_hydrology.py out/hydrology
```

The C++ executable exports daily water budgets, a monitored catchment/reach
history, reference nodes and map samples. The optional plotting script requires
Matplotlib/NumPy and writes `history.png` and `maps.png`. New hydrology fields
also pass through the existing C ABI/Godot field introspection. River geometry
stays at the initial base level when the active simulation cover refines;
see [the hydrology contract](docs/HYDROLOGY.md).

## Continuous integration

GitHub Actions deliberately separates the fast simulation-core job from the Godot integration job. The Godot job uses a feature build profile (`godot/build_profile.json`) so cold builds generate only the C++ wrappers actually used by the adapter. It then runs a headless smoke test with pinned Godot 4.7.2.

The regular CI builds only the Linux Godot integration. Cross-platform packaging should be a release workflow rather than multiplying every pull-request build.

## Build Godot 4.7 adapter

The build pins `godot-cpp` to `10.0.0-rc2` and explicitly targets the Godot 4.7 extension API. `godot-cpp` is fetched at configure time unless it is present at `third_party/godot-cpp`.

```bash
cmake --preset godot-dev
cmake --build --preset godot-dev
ctest --preset godot-dev
```

Launch the viewer scene with Godot 4.7.x on `PATH` (override with `GODOT=/path/to/godot`):

```bash
make run
```

That builds `libworldsim_godot.so` if needed and starts `godot/project/main.tscn`. You can also open `godot/project/project.godot` in the editor. The current stable maintenance release at project finalization is Godot 4.7.2.

## C++ extension model

```cpp
class DiseaseModule final : public worldsim::ISimModule {
public:
    std::string_view id() const override { return "disease"; }
    void register_fields(worldsim::FieldRegistry&) override;
    void register_stores(worldsim::StateStoreRegistry&, const worldsim::FieldRegistry&) override;
    void register_systems(worldsim::Scheduler&, const worldsim::FieldRegistry&) override;
    void initialize(worldsim::WorldState&, const worldsim::FieldRegistry&) override;
};
```

No changes to `Simulation`, `WorldState`, LOD code, snapshot framing, or engine adapters are required merely to introduce the domain. See `docs/ADDING_MODULES.md`.

## Engine boundary

`worldsim_c` is a shared library with a C ABI. It exposes:

- active cells and spherical positions;
- field registry introspection (keys, units, intensive/extensive semantics);
- field arrays aligned to active-cell order;
- focus/LOD control;
- scheduled field impulses;
- pending simulation events;
- snapshot save/load.

The Godot adapter is a client of the same simulation kernel and additionally exposes generic `get_field_descriptors()` and `get_field_values(key)` methods, so a newly registered field can be visualized without adding a hard-coded C++ accessor.

## Architectural references

The architecture is informed by established patterns rather than copied from a single engine:

- Unreal Mass separates data-oriented entities/processors and supports simulation LOD distinct from rendering concerns: <https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine>
- S2 demonstrates a hierarchical planet-scale cell model with stable parent/child relationships: <https://s2geometry.io/devguide/s2cell_hierarchy>
- Cohort/super-individual ecosystem modeling is used in global ecosystem models such as Madingley: <https://doi.org/10.1371/journal.pbio.1001841>
- Godot 4.7 GDExtension descriptor contract: <https://docs.godotengine.org/en/4.7/engine_details/engine_api/gdextension/gdextension_file.html>
- Official `godot-cpp` bindings and versioned API targeting: <https://github.com/godotengine/godot-cpp>

## Important scope boundary

This archive provides the architectural foundation and a working multi-domain vertical slice. A production world still requires domain work: calibrated atmospheric/ocean models, explicit soil biogeochemistry, calibrated plant traits/phenology/disturbance and long-range dispersal, species databases and behavior, aquatic ecology, population dispersal/migration, eventual settlement/population models, transport, markets, institutions, diplomacy, warfare, epidemiology, content pipelines, persistence scaling, and profiling on the target hardware. Those belong behind the module/state-store/system contracts already present; they are not hidden TODOs that require redesigning the kernel.

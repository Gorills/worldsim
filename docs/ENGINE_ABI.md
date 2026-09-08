# Engine ABI

`include/worldsim/c_api.h` is the engine-neutral binary boundary. It avoids STL types, exceptions, and C++ object layouts.

## Lifetime

```c
ws_handle* h = ws_create_default(seed);
/* use h */
ws_destroy(h);
```

Every mutating C call catches C++ exceptions. On failure it returns `0` (or zero length where documented) and stores diagnostic text available through `ws_last_error()`.

## Active-cell alignment

Call `ws_cell_count()` and `ws_copy_cells()` to obtain the current active cover. `ws_copy_field_values()` returns values in the same canonical active-cell order.

Do not retain array indices across a simulation step that may change LOD. Retain `uint64_t cell_id` if identity is required, and reacquire arrays after stepping.

## Field discovery

The ABI is not hard-coded to climate/ecology/magic names:

```c
size_t count = ws_field_count(h);
for (uint32_t id = 0; id < count; ++id) {
    size_t n = ws_field_key(h, id, NULL, 0);
    /* allocate n bytes, then call ws_field_key again */
}
```

`ws_field_semantics()` tells the consumer whether a value is intensive or extensive. A renderer can divide an extensive field by `ws_cell_v1.area_m2` when it wants a spatial density, if that unit conversion is meaningful for the field.

## Events

`ws_copy_events()` copies stable event headers. `type_hash` is FNV-1a of the event type string; `ws_event_type()` returns the full string for an event index. Events remain pending until `ws_clear_events()` (or an engine-specific drain operation) is called.

## Commands

The included ABI command is a scheduled field impulse. It is deliberately small; production gameplay should add typed versioned commands rather than convert every action into a scalar mutation.

## Snapshot files

`ws_save_snapshot_file()` and `ws_load_snapshot_file()` expose the authoritative snapshot. Save compatibility is strict by design: schema/config mismatches fail instead of corrupting state silently. The current epoch is 18. A rejected load leaves the simulation unchanged, including its cover, queued commands and events. Non-finite command deltas and null load paths fail at the API boundary.

Climate v2 fields are discovered through the same generic registry API. The
authoritative atmospheric-water, land/ocean heat and ocean-water reservoirs
live in `ClimateStore`; scalar field arrays are active-cover projections.

Hydrology fields are available through the existing registry/field-array API.
`hydrology.surface_water_m3` is a projection of `HydrologyStore`, not an
independently writable water reservoir; surface diagnostics are overwritten by
hydrology/geology/LOD projection. Add surface water through the C++ domain
store API; no new typed surface-water command is exposed by the C ABI in this
slice. Snow, root-zone water and groundwater remain authoritative fields.

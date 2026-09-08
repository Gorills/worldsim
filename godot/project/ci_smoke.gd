extends SceneTree

func _initialize() -> void:
    var sim := WorldSimulationNode.new()
    sim.initialize(123456789)
    if not sim.get_last_error().is_empty():
        push_error("WorldSim init failed: %s" % sim.get_last_error())
        quit(1)
        return

    sim.step_hours(24)
    if sim.get_tick() != 24:
        push_error("WorldSim tick mismatch: expected 24, got %d" % sim.get_tick())
        quit(2)
        return

    var packet := sim.get_render_packet()
    if packet.is_empty() or not packet.has("positions") or packet["positions"].is_empty():
        push_error("WorldSim render packet is empty")
        quit(3)
        return

    var descriptors := sim.get_field_descriptors()
    if descriptors.is_empty():
        push_error("WorldSim field descriptors are empty")
        quit(4)
        return

    var temperatures := sim.get_field_values("climate.surface_temperature_k")
    if temperatures.size() != packet["positions"].size():
        push_error("WorldSim generic field values are not aligned with render packet")
        quit(5)
        return
    var packet_temperatures: PackedFloat32Array = packet["temperature_c"]
    if absf(float(temperatures[0]) - 273.15 - float(packet_temperatures[0])) > 0.001:
        push_error("WorldSim render packet and generic field order diverged")
        quit(6)
        return

    for key in ["hydrology.snow_water_m3", "hydrology.groundwater_m3",
                "hydrology.surface_water_m3", "hydrology.river_discharge_m3_day",
                "hydrology.flooded_fraction"]:
        var water_values := sim.get_field_values(key)
        if water_values.size() != packet["positions"].size():
            push_error("Hydrology field is missing or misaligned: %s" % key)
            quit(23)
            return
        for water_value in water_values:
            if not is_finite(water_value) or water_value < 0.0:
                push_error("Hydrology field contains invalid state: %s" % key)
                quit(24)
                return

    # A diagnostic elevation map must read the current adaptive world. A seed-only
    # TerrainGenerator silently hides geological evolution and field commands.
    sim.set_focus_direction(Vector3(1.0, 0.2, 0.3))
    sim.step_hours(1)
    var adaptive_packet := sim.get_render_packet()
    var elevation := sim.get_field_values("geography.elevation_m")
    for i in range(elevation.size()):
        sim.schedule_field_impulse(
            adaptive_packet["cell_id_hi"][i], adaptive_packet["cell_id_lo"][i],
            "geography.elevation_m", 1234.0 - elevation[i]
        )
    sim.step_hours(1)
    var authoritative_map := sim.sample_terrain_equirectangular(16, 8)
    if authoritative_map.size() != 16 * 8:
        push_error("Authoritative elevation map is missing")
        quit(21)
        return
    for value in authoritative_map:
        if absf(value - 1234.0) > 0.01:
            push_error("Elevation map ignores authoritative adaptive geography")
            quit(22)
            return

    sim.initialize_terrain_world(42)
    if not sim.get_last_error().is_empty():
        push_error("Terrain world init failed: %s" % sim.get_last_error())
        quit(7)
        return

    var terrain_packet := sim.get_render_packet()
    if not terrain_packet.is_empty() or sim.get_last_error().is_empty():
        push_error("Terrain-only render packet did not fail safely")
        quit(8)
        return

    var patch := sim.sample_terrain_patch(0.0, 0.0, 8.0, 17)
    if patch.size() != 17 * 17:
        push_error("Terrain patch size mismatch: %d" % patch.size())
        quit(9)
        return
    if sim.sample_terrain_height(0.0, 0.0) <= 0.0:
        push_error("Terrain continent center is not above sea level")
        quit(10)
        return
    if sim.sample_terrain_height(8000000.0, 4000000.0) >= 0.0:
        push_error("Remote terrain sample is not ocean floor")
        quit(11)
        return

    var walker_origin_direction := sim.projected_to_direction(0.0, 0.0)
    if absf(walker_origin_direction.length() - 1.0) > 0.0001:
        push_error("Projected walker coordinates did not map to a unit sphere direction")
        quit(25)
        return

    var preview_map := sim.sample_preview_terrain_equirectangular(64, 32)
    if preview_map.size() != 64 * 32:
        push_error("Preview terrain map size mismatch: %d" % preview_map.size())
        quit(26)
        return
    var preview_has_land := false
    var preview_has_ocean := false
    for preview_height in preview_map:
        preview_has_land = preview_has_land or preview_height >= 0.0
        preview_has_ocean = preview_has_ocean or preview_height < 0.0
    if !preview_has_land or !preview_has_ocean:
        push_error("Preview terrain map does not contain both ocean and land")
        quit(27)
        return

    var global_map := sim.sample_terrain_equirectangular(64, 32)
    if global_map.size() != 64 * 32:
        push_error("Global terrain map size mismatch: %d" % global_map.size())
        quit(12)
        return
    var map_min := float(global_map[0])
    var map_max := map_min
    for value_variant in global_map:
        var value := float(value_variant)
        if value != value or absf(value) > 20000.0:
            push_error("Global terrain map contains a non-finite or out-of-range elevation")
            quit(13)
            return
        map_min = minf(map_min, value)
        map_max = maxf(map_max, value)
    if map_min >= 0.0 or map_max <= 0.0:
        push_error("Global terrain map does not contain both ocean and land")
        quit(14)
        return

    var tectonics: Dictionary = sim.sample_tectonics_equirectangular(64, 32)
    if (
        tectonics.is_empty() or
        !tectonics.has("plate_id") or
        !tectonics.has("forcing") or
        !tectonics.has("uplift_forcing") or
        !tectonics.has("divergence_forcing") or
        !tectonics.has("crust_affinity") or
        !tectonics.has("macro_elevation_m")
    ):
        push_error("Global tectonic debug map is missing")
        quit(15)
        return
    if int(tectonics.get("plate_count", 0)) != 16:
        push_error("Unexpected tectonic plate count: %d" % int(tectonics.get("plate_count", 0)))
        quit(16)
        return
    var plate_ids: PackedInt32Array = tectonics["plate_id"]
    var forcing: PackedFloat32Array = tectonics["forcing"]
    var uplift_forcing: PackedFloat32Array = tectonics["uplift_forcing"]
    var divergence_forcing: PackedFloat32Array = tectonics["divergence_forcing"]
    var crust_affinity: PackedFloat32Array = tectonics["crust_affinity"]
    var macro_elevation: PackedFloat32Array = tectonics["macro_elevation_m"]
    if (
        plate_ids.size() != 64 * 32 or
        forcing.size() != 64 * 32 or
        uplift_forcing.size() != 64 * 32 or
        divergence_forcing.size() != 64 * 32 or
        crust_affinity.size() != 64 * 32 or
        macro_elevation.size() != 64 * 32
    ):
        push_error("Global tectonic debug layer size mismatch")
        quit(17)
        return
    var seen_plates := {}
    var has_convergence := false
    var has_divergence := false
    var has_uplift_response := false
    var has_divergence_response := false
    var has_crust_transition := false
    var macro_min := float(macro_elevation[0])
    var macro_max := macro_min
    for i in range(plate_ids.size()):
        var plate_id := int(plate_ids[i])
        var force := float(forcing[i])
        var uplift := float(uplift_forcing[i])
        var divergence := float(divergence_forcing[i])
        var affinity := float(crust_affinity[i])
        var macro_m := float(macro_elevation[i])
        if (
            plate_id < 0 or
            plate_id >= 16 or
            force != force or
            absf(force) > 1.0 or
            uplift != uplift or
            uplift < 0.0 or
            uplift > 1.0 or
            divergence != divergence or
            divergence < 0.0 or
            divergence > 1.0 or
            affinity != affinity or
            affinity < 0.0 or
            affinity > 1.0 or
            macro_m != macro_m or
            macro_m < -6000.0 or
            macro_m > 6500.0
        ):
            push_error("Invalid tectonic debug sample")
            quit(18)
            return
        seen_plates[plate_id] = true
        has_convergence = has_convergence or force > 0.0001
        has_divergence = has_divergence or force < -0.0001
        has_uplift_response = has_uplift_response or uplift > 0.05
        has_divergence_response = has_divergence_response or divergence > 0.05
        has_crust_transition = has_crust_transition or (affinity > 0.05 and affinity < 0.95)
        macro_min = minf(macro_min, macro_m)
        macro_max = maxf(macro_max, macro_m)
    if (
        seen_plates.size() < 8 or
        !has_convergence or
        !has_divergence or
        !has_uplift_response or
        !has_divergence_response or
        !has_crust_transition or
        macro_min >= -3000.0 or
        macro_max <= 1000.0
    ):
        push_error("Tectonic debug map lacks expected plate/crust/macro variation")
        quit(19)
        return

    sim.set_focus_projected(0.0, 0.0)
    sim.step_hours(1)
    if not sim.get_last_error().is_empty():
        push_error("Terrain simulation step failed: %s" % sim.get_last_error())
        quit(20)
        return

    print("WORLDSIM_GODOT_SMOKE_OK tick=%d cells=%d fields=%d" % [
        sim.get_tick(), packet["positions"].size(), descriptors.size()
    ])
    sim.free()
    quit(0)

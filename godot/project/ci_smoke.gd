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

    var diagnostic_elevation := sim.sample_field_equirectangular(
        "geography.elevation_m", 16, 8, true
    )
    var terrain_elevation := sim.sample_terrain_equirectangular(16, 8)
    if diagnostic_elevation.size() != terrain_elevation.size():
        push_error("Generic diagnostic field map has the wrong size")
        quit(32)
        return
    for i in range(diagnostic_elevation.size()):
        if absf(float(diagnostic_elevation[i]) - float(terrain_elevation[i])) > 0.01:
            push_error("Generic elevation map disagrees with authoritative terrain map")
            quit(33)
            return

    var pixel_latitude := (0.5 - 0.5 / 8.0) * PI
    var pixel_longitude := (2.0 * 0.5 / 16.0 - 1.0) * PI
    var pixel_direction := Vector3(
        cos(pixel_latitude) * cos(pixel_longitude),
        cos(pixel_latitude) * sin(pixel_longitude),
        sin(pixel_latitude)
    )
    var inspected: Dictionary = sim.inspect_direction(pixel_direction)
    if (
        inspected.is_empty() or
        int(inspected.get("level", -1)) != 4 or
        float(inspected.get("area_m2", 0.0)) <= 0.0 or
        !(inspected.get("values", {}) as Dictionary).has("climate.surface_temperature_k")
    ):
        push_error("Direction inspector did not return aligned active-cell state")
        quit(34)
        return

    var vegetation_density := sim.sample_field_equirectangular(
        "ecology.vegetation_carbon_kg", 16, 8, true
    )
    var inspected_values: Dictionary = inspected["values"]
    var expected_density := (
        float(inspected_values["ecology.vegetation_carbon_kg"])
        / float(inspected["area_m2"])
    )
    if absf(float(vegetation_density[0]) - expected_density) > maxf(
        1.0e-12, absf(expected_density) * 1.0e-6
    ):
        push_error("Extensive diagnostic field was not normalized by active-cell area")
        quit(35)
        return

    var base_lod := sim.sample_lod_equirectangular(32, 16)
    for level in base_lod:
        if level != 4:
            push_error("Unfocused full world did not start on uniform base LOD")
            quit(36)
            return

    # A diagnostic elevation map must read the current adaptive world. A seed-only
    # TerrainGenerator silently hides geological evolution and field commands.
    sim.set_focus_direction(Vector3(1.0, 0.2, 0.3))
    sim.step_hours(1)
    var focused_lod := sim.sample_lod_equirectangular(32, 16)
    var focused_min_level := 99
    var focused_max_level := -1
    for level in focused_lod:
        focused_min_level = mini(focused_min_level, level)
        focused_max_level = maxi(focused_max_level, level)
    if focused_min_level != 4 or focused_max_level <= 4:
        push_error("LOD diagnostic map did not expose focused refinement")
        quit(37)
        return
    var adaptive_packet := sim.get_render_packet()
    var elevation := sim.get_field_values("geography.elevation_m")
    var render_height_before := sim.sample_terrain_height(0.0, 0.0)
    var terrain_revision_before := sim.get_terrain_revision()
    for i in range(elevation.size()):
        sim.schedule_field_impulse(
            adaptive_packet["cell_id_hi"][i], adaptive_packet["cell_id_lo"][i],
            "geography.elevation_m", 1234.0 - elevation[i]
        )
    sim.step_hours(1)
    if sim.get_terrain_revision() <= terrain_revision_before:
        push_error("Terrain revision did not track an authoritative elevation change")
        quit(28)
        return
    var render_height_after := sim.sample_terrain_height(0.0, 0.0)
    if absf(render_height_after - render_height_before) < 1.0:
        push_error("Walking terrain height ignores authoritative geography")
        quit(29)
        return
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
    var terrain_origin_height := sim.sample_terrain_height(0.0, 0.0)
    if absf(float(patch[8 * 17 + 8]) - terrain_origin_height) > 0.01:
        push_error("Terrain point and patch reconstruction disagree")
        quit(30)
        return
    var left_chunk := sim.sample_terrain_patch(0.0, 0.0, 8.0, 33)
    var right_chunk := sim.sample_terrain_patch(256.0, 0.0, 8.0, 33)
    for z in range(33):
        if absf(float(left_chunk[z * 33 + 32]) - float(right_chunk[z * 33])) > 0.01:
            push_error("Reconstructed terrain has a chunk seam")
            quit(31)
            return
    if terrain_origin_height <= 0.0:
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

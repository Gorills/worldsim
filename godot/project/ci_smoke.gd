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

    sim.set_focus_projected(0.0, 0.0)
    sim.step_hours(1)
    if not sim.get_last_error().is_empty():
        push_error("Terrain simulation step failed: %s" % sim.get_last_error())
        quit(15)
        return

    print("WORLDSIM_GODOT_SMOKE_OK tick=%d cells=%d fields=%d" % [
        sim.get_tick(), packet["positions"].size(), descriptors.size()
    ])
    sim.free()
    quit(0)

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

    print("WORLDSIM_GODOT_SMOKE_OK tick=%d cells=%d fields=%d" % [
        sim.get_tick(), packet["positions"].size(), descriptors.size()
    ])
    sim.free()
    quit(0)

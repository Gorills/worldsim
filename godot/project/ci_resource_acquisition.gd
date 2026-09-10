extends SceneTree

const EAST_M := 5564875.0
const NORTH_M := -1795925.0

func _initialize() -> void:
    var sim := SurvivalSimulationNode.new()
    root.add_child(sim)
    sim.initialize_survival(42)
    if !sim.get_last_error().is_empty():
        push_error(sim.get_last_error())
        quit(1)
        return
    var local := sim.get_local_resources(EAST_M, NORTH_M)
    var inventory := sim.get_inventory()
    if local.is_empty() or inventory.is_empty():
        push_error("resource query returned no state")
        quit(1)
        return
    var before := float(local.get("stone", 0.0))
    if before < 2.0:
        push_error("walking spawn has no stone")
        quit(1)
        return
    if !sim.collect_resource_at(EAST_M, NORTH_M, "stone", 1.0):
        push_error(sim.get_last_error())
        quit(1)
        return
    local = sim.get_local_resources(EAST_M, NORTH_M)
    inventory = sim.get_inventory()
    if absf(float(local.get("stone", 0.0)) - (before - 1.0)) > 0.000001:
        push_error("stone source transfer mismatch")
        quit(1)
        return
    if absf(float(inventory.get("stone", 0.0)) - 1.0) > 0.000000001:
        push_error("stone inventory transfer mismatch")
        quit(1)
        return
    print("WORLDSIM_RESOURCE_ACQUISITION_OK")
    quit(0)

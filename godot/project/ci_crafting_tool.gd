extends SceneTree

const SPAWN_EAST_M := 5_564_875.0
const SPAWN_NORTH_M := -1_795_925.0
const SEARCH_STEP_M := 250_000.0
const SEARCH_RADIUS := 8

func _find_crafting_location(sim: SurvivalSimulationNode) -> Vector2:
    for radius in range(SEARCH_RADIUS + 1):
        for dz in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if radius > 0 and abs(dx) != radius and abs(dz) != radius:
                    continue
                var east_m := SPAWN_EAST_M + float(dx) * SEARCH_STEP_M
                var north_m := SPAWN_NORTH_M + float(dz) * SEARCH_STEP_M
                var local := sim.get_local_resources(east_m, north_m)
                if local.is_empty():
                    continue
                if (
                    float(local.get("wood", 0.0)) >= 4.0
                    and float(local.get("stone", 0.0)) >= 1.0
                ):
                    return Vector2(east_m, north_m)
    return Vector2(INF, INF)

func _initialize() -> void:
    var sim := SurvivalSimulationNode.new()
    root.add_child(sim)
    sim.initialize_survival(42)
    if !sim.get_last_error().is_empty():
        push_error(sim.get_last_error())
        quit(1)
        return

    var location := _find_crafting_location(sim)
    if !is_finite(location.x) or !is_finite(location.y):
        push_error("no world-provided wood-and-stone crafting location found")
        quit(2)
        return

    var local_before := sim.get_local_resources(location.x, location.y)
    var wood_before := float(local_before.get("wood", 0.0))
    var stone_before := float(local_before.get("stone", 0.0))

    var bare_wood := sim.gather_resource_at(location.x, location.y, "wood")
    if absf(bare_wood - 1.0) > 0.000000001:
        push_error("bare-hand wood gather did not realize 1 kg")
        quit(3)
        return
    var bare_stone := sim.gather_resource_at(location.x, location.y, "stone")
    if absf(bare_stone - 1.0) > 0.000000001:
        push_error("stone gather did not realize 1 kg")
        quit(4)
        return

    var gathered := sim.get_inventory()
    if (
        absf(float(gathered.get("wood", 0.0)) - 1.0) > 0.000000001
        or absf(float(gathered.get("stone", 0.0)) - 1.0) > 0.000000001
    ):
        push_error("gathered crafting inputs are not authoritative inventory")
        quit(5)
        return

    if !sim.craft_stone_axe():
        push_error(sim.get_last_error())
        quit(6)
        return
    if !sim.has_stone_axe():
        push_error("crafted stone axe is not authoritative tool state")
        quit(7)
        return

    var crafted_inventory := sim.get_inventory()
    if (
        absf(float(crafted_inventory.get("wood", 0.0))) > 0.000000001
        or absf(float(crafted_inventory.get("stone", 0.0))) > 0.000000001
    ):
        push_error("stone axe craft did not debit exact inputs")
        quit(8)
        return

    if sim.craft_stone_axe():
        push_error("duplicate stone axe craft succeeded")
        quit(9)
        return
    if !sim.has_stone_axe():
        push_error("duplicate craft failure changed tool state")
        quit(10)
        return

    var axe_wood := sim.gather_resource_at(location.x, location.y, "wood")
    if absf(axe_wood - 3.0) > 0.000000001:
        push_error("stone axe wood gather did not realize 3 kg")
        quit(11)
        return

    var local_after := sim.get_local_resources(location.x, location.y)
    var inventory_after := sim.get_inventory()
    if absf(float(local_after.get("wood", 0.0)) - (wood_before - 4.0)) > 0.000001:
        push_error("tool-aware wood gather did not debit exact world source")
        quit(12)
        return
    if absf(float(local_after.get("stone", 0.0)) - (stone_before - 1.0)) > 0.000001:
        push_error("crafting path changed world stone beyond gathered input")
        quit(13)
        return
    if absf(float(inventory_after.get("wood", 0.0)) - 3.0) > 0.000000001:
        push_error("tool-aware wood gather did not credit exact inventory")
        quit(14)
        return

    print(
        "WORLDSIM_CRAFTING_TOOL_OK east_m=%.0f north_m=%.0f wood_before=%.2f"
        % [location.x, location.y, wood_before]
    )
    quit(0)

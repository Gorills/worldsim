extends SceneTree

const SPAWN_EAST_M := 5_564_875.0
const SPAWN_NORTH_M := -1_795_925.0
const SEARCH_STEP_M := 250_000.0
const SEARCH_RADIUS := 8

func _find_campsite_location(sim: SurvivalSimulationNode) -> Vector2:
    for radius in range(SEARCH_RADIUS + 1):
        for dz in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if radius > 0 and abs(dx) != radius and abs(dz) != radius:
                    continue
                var east_m := SPAWN_EAST_M + float(dx) * SEARCH_STEP_M
                var north_m := SPAWN_NORTH_M + float(dz) * SEARCH_STEP_M
                var campsite := sim.get_campsite(east_m, north_m)
                var local := sim.get_local_resources(east_m, north_m)
                if campsite.is_empty() or local.is_empty():
                    continue
                if (
                    bool(campsite.get("suitable", false))
                    and float(local.get("wood", 0.0)) >= 10.0
                    and float(local.get("stone", 0.0)) >= 5.0
                ):
                    return Vector2(east_m, north_m)
    return Vector2(INF, INF)

func _fail(message: String, code: int) -> void:
    push_error(message)
    quit(code)

func _initialize() -> void:
    var sim := SurvivalSimulationNode.new()
    root.add_child(sim)
    sim.initialize_survival(42)
    if !sim.get_last_error().is_empty():
        _fail(sim.get_last_error(), 1)
        return

    var location := _find_campsite_location(sim)
    if !is_finite(location.x) or !is_finite(location.y):
        _fail("no suitable world-provided campsite location found", 2)
        return

    # Empty authoritative player state -> gathered materials -> prior-slice axe.
    if absf(sim.gather_resource_at(location.x, location.y, "wood") - 1.0) > 0.000000001:
        _fail("initial wood gather failed", 3)
        return
    if absf(sim.gather_resource_at(location.x, location.y, "stone") - 1.0) > 0.000000001:
        _fail("initial stone gather failed", 4)
        return
    if !sim.craft_stone_axe() or !sim.has_stone_axe():
        _fail("stone axe prerequisite failed", 5)
        return

    for _i in range(3):
        if absf(sim.gather_resource_at(location.x, location.y, "wood") - 3.0) > 0.000000001:
            _fail("axe wood gather failed", 6)
            return
    for _i in range(4):
        if absf(sim.gather_resource_at(location.x, location.y, "stone") - 1.0) > 0.000000001:
            _fail("stone gather failed", 7)
            return

    var before_build := sim.get_inventory()
    if (
        absf(float(before_build.get("wood", 0.0)) - 9.0) > 0.000000001
        or absf(float(before_build.get("stone", 0.0)) - 4.0) > 0.000000001
    ):
        _fail("world-gathered construction inventory is wrong", 8)
        return

    if !sim.build_basic_shelter_at(location.x, location.y):
        _fail(sim.get_last_error(), 9)
        return
    if !sim.build_campfire_at(location.x, location.y):
        _fail(sim.get_last_error(), 10)
        return

    var built := sim.get_campsite(location.x, location.y)
    if !bool(built.get("shelter", false)) or !bool(built.get("campfire", false)):
        _fail("authoritative campsite build state missing", 11)
        return
    var after_build := sim.get_inventory()
    if (
        absf(float(after_build.get("wood", 0.0)) - 3.0) > 0.000000001
        or absf(float(after_build.get("stone", 0.0))) > 0.000000001
    ):
        _fail("campsite construction did not debit exact inventory", 12)
        return

    if sim.build_basic_shelter_at(location.x, location.y):
        _fail("duplicate shelter build succeeded", 13)
        return
    if sim.get_last_error().is_empty():
        _fail("duplicate shelter rejection had no player-facing reason", 14)
        return

    if !sim.add_campfire_fuel_at(location.x, location.y, 1.0):
        _fail(sim.get_last_error(), 15)
        return
    var fueled := sim.get_campsite(location.x, location.y)
    if absf(float(fueled.get("campfire_fuel_kg", 0.0)) - 1.0) > 0.000000001:
        _fail("campfire fuel transfer failed", 16)
        return
    if absf(float(sim.get_inventory().get("wood", 0.0)) - 2.0) > 0.000000001:
        _fail("campfire fuel did not debit carried wood", 17)
        return

    if !sim.set_campfire_lit_at(location.x, location.y, true):
        _fail(sim.get_last_error(), 18)
        return
    sim.step_hours(1)
    var burning := sim.get_campsite(location.x, location.y)
    if !bool(burning.get("campfire_lit", false)):
        _fail("campfire extinguished before fuel was exhausted", 19)
        return
    if absf(float(burning.get("campfire_fuel_kg", 0.0)) - 0.75) > 0.000000001:
        _fail("campfire did not burn 0.25 kg in one simulated hour", 20)
        return

    sim.step_hours(3)
    var exhausted := sim.get_campsite(location.x, location.y)
    if bool(exhausted.get("campfire_lit", true)):
        _fail("campfire did not extinguish at zero fuel", 21)
        return
    if absf(float(exhausted.get("campfire_fuel_kg", -1.0))) > 0.000000001:
        _fail("campfire fuel did not clamp to zero", 22)
        return

    print(
        "WORLDSIM_SHELTER_FIRE_OK east_m=%.0f north_m=%.0f"
        % [location.x, location.y]
    )
    quit(0)
extends "res://scripts/world_viewer.gd"

const RESOURCE_KEYS := [
    "fresh_water",
    "plant_food",
    "wood",
    "stone",
    "metal_ore",
]
const RESOURCE_NAME_KEYS := [
    "SURVIVAL_RESOURCE_FRESH_WATER",
    "SURVIVAL_RESOURCE_PLANT_FOOD",
    "SURVIVAL_RESOURCE_WOOD",
    "SURVIVAL_RESOURCE_STONE",
    "SURVIVAL_RESOURCE_METAL_ORE",
]
const RESOURCE_UNITS := ["L", "kg", "kg", "kg", "kg"]

# The base viewer is a diagnostic host and advances one simulated hour every
# 0.25 real seconds. Survival uses a playable clock: one simulated hour per
# real minute. Natural systems still run through the same authoritative step.
const SURVIVAL_SIM_STEP_INTERVAL_S := 60.0

const ACTION_GATHER_BASE := 100
const ACTION_CRAFT_STONE_AXE := 200
const ACTION_BUILD_SHELTER := 300
const ACTION_BUILD_CAMPFIRE := 301
const ACTION_ADD_CAMPFIRE_FUEL := 302
const ACTION_TOGGLE_CAMPFIRE := 303

@onready var resource_status: Label = $HUD/ResourcePanel/Margin/ResourceStatus

var resource_status_elapsed := 0.0
var interaction_message := ""
var action_menu: PopupMenu
var restore_mouse_capture_after_menu := false

func _ready() -> void:
    super._ready()
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        resource_status.text = tr("SURVIVAL_ERROR_SIM_UNAVAILABLE")
        return
    survival_sim.initialize_survival(42)
    if !survival_sim.get_last_error().is_empty():
        resource_status.text = tr("SURVIVAL_ERROR_STATE") % survival_sim.get_last_error()
        return
    terrain_revision = sim.get_terrain_revision()
    surface_revision = sim.get_surface_revision()
    pending_surface_revision = surface_revision
    _create_action_menu()
    help.text = tr("SURVIVAL_CONTROLS")
    _update_resource_status()

func _create_action_menu() -> void:
    action_menu = PopupMenu.new()
    action_menu.name = "SurvivalActions"
    action_menu.allow_search = true
    action_menu.id_pressed.connect(_execute_action)
    action_menu.popup_hide.connect(_on_action_menu_closed)
    add_child(action_menu)

# Keep rendering/streaming cadence unchanged while replacing only the diagnostic
# simulation clock inherited from world_viewer.gd.
func _process(delta: float) -> void:
    for _i in range(MAX_CHUNKS_PER_FRAME):
        if pending_chunks.is_empty():
            break
        _create_chunk(pending_chunks.pop_front())

    sim_focus_elapsed += delta
    if sim_focus_elapsed >= SURVIVAL_SIM_STEP_INTERVAL_S:
        sim_focus_elapsed = fmod(sim_focus_elapsed, SURVIVAL_SIM_STEP_INTERVAL_S)
        var east_m := origin_east_m + float(player.position.x)
        var north_m := origin_north_m - float(player.position.z)
        var old_origin_height := origin_height_m
        var old_ground_height := sim.sample_terrain_height(east_m, north_m)
        var was_grounded := !survey_flight_enabled and player.is_on_floor()
        sim.set_focus_projected(east_m, north_m)
        sim.step_hours(1)
        _refresh_terrain_revision(old_origin_height, old_ground_height, was_grounded)
        _observe_surface_revision()

    _refresh_surface_revision(delta)

    status_elapsed += delta
    if status_elapsed >= 0.25:
        status_elapsed = 0.0
        _update_status()

    resource_status_elapsed += delta
    if resource_status_elapsed >= 0.25:
        resource_status_elapsed = 0.0
        _update_resource_status()

    _update_minimap_marker()

func _unhandled_input(event: InputEvent) -> void:
    if (
        event.is_action_pressed("survival_interact")
        and !survey_flight_enabled
        and action_menu != null
        and !action_menu.visible
    ):
        _open_action_menu()
        get_viewport().set_input_as_handled()
        return
    super._unhandled_input(event)

func _player_projected_position() -> Vector2:
    return Vector2(
        origin_east_m + float(player.position.x),
        origin_north_m - float(player.position.z)
    )

func _survival_snapshot() -> Dictionary:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = tr("SURVIVAL_ERROR_SIM_UNAVAILABLE")
        return {}
    var position := _player_projected_position()
    var local := survival_sim.get_local_resources(position.x, position.y)
    if local.is_empty():
        interaction_message = tr("SURVIVAL_ERROR_STATE") % survival_sim.get_last_error()
        return {}
    var inventory := survival_sim.get_inventory()
    if inventory.is_empty():
        interaction_message = tr("SURVIVAL_ERROR_STATE") % survival_sim.get_last_error()
        return {}
    var campsite := survival_sim.get_campsite(position.x, position.y)
    if campsite.is_empty():
        interaction_message = tr("SURVIVAL_ERROR_STATE") % survival_sim.get_last_error()
        return {}
    return {
        "position": position,
        "local": local,
        "inventory": inventory,
        "campsite": campsite,
        "has_axe": survival_sim.has_stone_axe(),
    }

func _open_action_menu() -> void:
    var snapshot := _survival_snapshot()
    if snapshot.is_empty():
        _update_resource_status()
        return
    _rebuild_action_menu(snapshot)
    restore_mouse_capture_after_menu = Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
    if DisplayServer.get_name() != "headless":
        Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
    action_menu.title = tr("SURVIVAL_MENU_TITLE")
    action_menu.popup_centered_clamped(Vector2i(760, 520))
    for index in range(action_menu.item_count):
        if !action_menu.is_item_separator(index) and !action_menu.is_item_disabled(index):
            action_menu.set_focused_item(index)
            break

func _on_action_menu_closed() -> void:
    if restore_mouse_capture_after_menu and DisplayServer.get_name() != "headless":
        Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
    restore_mouse_capture_after_menu = false

func _add_menu_action(label: String, action_id: int, enabled: bool) -> void:
    action_menu.add_item(label, action_id)
    action_menu.set_item_disabled(action_menu.item_count - 1, !enabled)

func _rebuild_action_menu(snapshot: Dictionary = {}) -> void:
    if snapshot.is_empty():
        snapshot = _survival_snapshot()
    action_menu.clear()
    if snapshot.is_empty():
        return

    var local: Dictionary = snapshot["local"]
    var inventory: Dictionary = snapshot["inventory"]
    var campsite: Dictionary = snapshot["campsite"]
    var has_axe := bool(snapshot["has_axe"])

    action_menu.add_separator(tr("SURVIVAL_MENU_GATHER"))
    for index in range(RESOURCE_KEYS.size()):
        var key: String = RESOURCE_KEYS[index]
        var available := float(local.get(key, 0.0))
        _add_menu_action(
            tr("SURVIVAL_ACTION_GATHER") % [
                tr(RESOURCE_NAME_KEYS[index]),
                available,
                RESOURCE_UNITS[index],
            ],
            ACTION_GATHER_BASE + index,
            available > 0.0
        )

    action_menu.add_separator(tr("SURVIVAL_MENU_CRAFT"))
    if has_axe:
        _add_menu_action(tr("SURVIVAL_ACTION_AXE_OWNED"), ACTION_CRAFT_STONE_AXE, false)
    else:
        _add_menu_action(
            tr("SURVIVAL_ACTION_CRAFT_AXE"),
            ACTION_CRAFT_STONE_AXE,
            float(inventory.get("wood", 0.0)) >= 1.0
                and float(inventory.get("stone", 0.0)) >= 1.0
        )

    action_menu.add_separator(tr("SURVIVAL_MENU_CAMPSITE"))
    var suitable := bool(campsite.get("suitable", false))
    var shelter := bool(campsite.get("shelter", false))
    var campfire := bool(campsite.get("campfire", false))
    var campfire_lit := bool(campsite.get("campfire_lit", false))
    var campfire_fuel := float(campsite.get("campfire_fuel_kg", 0.0))

    if shelter:
        _add_menu_action(tr("SURVIVAL_ACTION_SHELTER_BUILT"), ACTION_BUILD_SHELTER, false)
    else:
        _add_menu_action(
            tr("SURVIVAL_ACTION_BUILD_SHELTER"),
            ACTION_BUILD_SHELTER,
            suitable
                and has_axe
                and float(inventory.get("wood", 0.0)) >= 6.0
                and float(inventory.get("stone", 0.0)) >= 2.0
        )

    if campfire:
        _add_menu_action(tr("SURVIVAL_ACTION_CAMPFIRE_BUILT"), ACTION_BUILD_CAMPFIRE, false)
    else:
        _add_menu_action(
            tr("SURVIVAL_ACTION_BUILD_CAMPFIRE"),
            ACTION_BUILD_CAMPFIRE,
            suitable and float(inventory.get("stone", 0.0)) >= 2.0
        )

    _add_menu_action(
        tr("SURVIVAL_ACTION_ADD_FUEL"),
        ACTION_ADD_CAMPFIRE_FUEL,
        campfire and float(inventory.get("wood", 0.0)) >= 1.0
    )
    _add_menu_action(
        tr("SURVIVAL_ACTION_EXTINGUISH_FIRE") if campfire_lit else tr("SURVIVAL_ACTION_LIGHT_FIRE"),
        ACTION_TOGGLE_CAMPFIRE,
        campfire and (campfire_lit or campfire_fuel > 0.0)
    )

func _execute_action(action_id: int) -> void:
    if action_id >= ACTION_GATHER_BASE and action_id < ACTION_GATHER_BASE + RESOURCE_KEYS.size():
        _gather_resource(action_id - ACTION_GATHER_BASE)
    elif action_id == ACTION_CRAFT_STONE_AXE:
        _craft_stone_axe()
    elif action_id == ACTION_BUILD_SHELTER:
        _build_basic_shelter()
    elif action_id == ACTION_BUILD_CAMPFIRE:
        _build_campfire()
    elif action_id == ACTION_ADD_CAMPFIRE_FUEL:
        _add_campfire_fuel()
    elif action_id == ACTION_TOGGLE_CAMPFIRE:
        _toggle_campfire()
    _update_resource_status()

func _gather_resource(resource_index: int) -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = tr("SURVIVAL_ERROR_SIM_UNAVAILABLE")
        return
    var position := _player_projected_position()
    var key: String = RESOURCE_KEYS[resource_index]
    var realized := survival_sim.gather_resource_at(position.x, position.y, key)
    if realized > 0.0:
        interaction_message = tr("SURVIVAL_RESULT_GATHERED") % [
            realized,
            RESOURCE_UNITS[resource_index],
            tr(RESOURCE_NAME_KEYS[resource_index]),
        ]
    else:
        interaction_message = _localized_error(survival_sim.get_last_error())

func _craft_stone_axe() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = tr("SURVIVAL_ERROR_SIM_UNAVAILABLE")
        return
    if survival_sim.craft_stone_axe():
        interaction_message = tr("SURVIVAL_RESULT_AXE")
    else:
        interaction_message = _localized_error(survival_sim.get_last_error())

func _build_basic_shelter() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = tr("SURVIVAL_ERROR_SIM_UNAVAILABLE")
        return
    var position := _player_projected_position()
    if survival_sim.build_basic_shelter_at(position.x, position.y):
        interaction_message = tr("SURVIVAL_RESULT_SHELTER")
    else:
        interaction_message = _localized_error(survival_sim.get_last_error())

func _build_campfire() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = tr("SURVIVAL_ERROR_SIM_UNAVAILABLE")
        return
    var position := _player_projected_position()
    if survival_sim.build_campfire_at(position.x, position.y):
        interaction_message = tr("SURVIVAL_RESULT_CAMPFIRE")
    else:
        interaction_message = _localized_error(survival_sim.get_last_error())

func _add_campfire_fuel() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = tr("SURVIVAL_ERROR_SIM_UNAVAILABLE")
        return
    var position := _player_projected_position()
    if survival_sim.add_campfire_fuel_at(position.x, position.y, 1.0):
        interaction_message = tr("SURVIVAL_RESULT_FUEL")
    else:
        interaction_message = _localized_error(survival_sim.get_last_error())

func _toggle_campfire() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = tr("SURVIVAL_ERROR_SIM_UNAVAILABLE")
        return
    var position := _player_projected_position()
    var campsite := survival_sim.get_campsite(position.x, position.y)
    if campsite.is_empty():
        interaction_message = _localized_error(survival_sim.get_last_error())
        return
    var target_lit := !bool(campsite.get("campfire_lit", false))
    if survival_sim.set_campfire_lit_at(position.x, position.y, target_lit):
        interaction_message = (
            tr("SURVIVAL_RESULT_FIRE_LIT")
            if target_lit
            else tr("SURVIVAL_RESULT_FIRE_OUT")
        )
    else:
        interaction_message = _localized_error(survival_sim.get_last_error())

func _localized_error(error: String) -> String:
    match error:
        "resource is unavailable at player location":
            return tr("SURVIVAL_ERROR_RESOURCE_UNAVAILABLE")
        "insufficient materials for stone axe":
            return tr("SURVIVAL_ERROR_AXE_MATERIALS")
        "stone axe already crafted":
            return tr("SURVIVAL_ERROR_AXE_DUP")
        "campsite requires non-flooded land":
            return tr("SURVIVAL_ERROR_SITE")
        "basic shelter already exists at campsite":
            return tr("SURVIVAL_ERROR_SHELTER_DUP")
        "basic shelter requires stone axe":
            return tr("SURVIVAL_ERROR_SHELTER_AXE")
        "insufficient materials for basic shelter":
            return tr("SURVIVAL_ERROR_SHELTER_MATERIALS")
        "campfire already exists at campsite":
            return tr("SURVIVAL_ERROR_FIRE_DUP")
        "insufficient stone for campfire":
            return tr("SURVIVAL_ERROR_FIRE_STONE")
        "campfire does not exist at campsite":
            return tr("SURVIVAL_ERROR_FIRE_MISSING")
        "insufficient carried wood for campfire fuel":
            return tr("SURVIVAL_ERROR_FIRE_WOOD")
        "campfire has no fuel":
            return tr("SURVIVAL_ERROR_FIRE_NO_FUEL")
        _:
            return tr("SURVIVAL_ERROR_GENERIC") % error

func _format_resources(values: Dictionary) -> String:
    var parts: Array[String] = []
    for index in range(RESOURCE_KEYS.size()):
        parts.append("%s %.2f %s" % [
            tr(RESOURCE_NAME_KEYS[index]),
            float(values.get(RESOURCE_KEYS[index], 0.0)),
            RESOURCE_UNITS[index],
        ])
    return "  |  ".join(parts)

func _update_resource_status() -> void:
    var snapshot := _survival_snapshot()
    if snapshot.is_empty():
        if !interaction_message.is_empty():
            resource_status.text = interaction_message
        return

    var local: Dictionary = snapshot["local"]
    var inventory: Dictionary = snapshot["inventory"]
    var campsite: Dictionary = snapshot["campsite"]
    var axe_text := (
        tr("SURVIVAL_STATE_OWNED")
        if bool(snapshot["has_axe"])
        else tr("SURVIVAL_STATE_NOT_CRAFTED")
    )
    var shelter_text := (
        tr("SURVIVAL_STATE_BUILT")
        if bool(campsite.get("shelter", false))
        else tr("SURVIVAL_STATE_MISSING")
    )
    var fire_text := tr("SURVIVAL_STATE_MISSING")
    if bool(campsite.get("campfire", false)):
        fire_text = (
            tr("SURVIVAL_STATE_LIT")
            if bool(campsite.get("campfire_lit", false))
            else tr("SURVIVAL_STATE_UNLIT")
        )
    var suitable_text := (
        tr("SURVIVAL_STATE_YES")
        if bool(campsite.get("suitable", false))
        else tr("SURVIVAL_STATE_NO")
    )

    var lines: Array[String] = [
        tr("SURVIVAL_HUD_REGIONAL") % _format_resources(local),
        tr("SURVIVAL_HUD_CARRIED") % _format_resources(inventory),
        tr("SURVIVAL_HUD_TOOL") % axe_text,
        tr("SURVIVAL_HUD_SITE") % [
            suitable_text,
            shelter_text,
            fire_text,
            float(campsite.get("campfire_fuel_kg", 0.0)),
        ],
        tr("SURVIVAL_HUD_INTERACT"),
        tr("SURVIVAL_HUD_REGIONAL_NOTE"),
    ]
    if !interaction_message.is_empty():
        lines.append(interaction_message)
    resource_status.text = "\n".join(lines)

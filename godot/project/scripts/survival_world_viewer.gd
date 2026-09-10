extends "res://scripts/world_viewer.gd"

const RESOURCE_KEYS := [
    "fresh_water",
    "plant_food",
    "wood",
    "stone",
    "metal_ore",
]
const RESOURCE_NAMES := [
    "Fresh water",
    "Plant food",
    "Wood",
    "Stone",
    "Metal ore",
]
const RESOURCE_UNITS := ["L", "kg", "kg", "kg", "kg"]

@onready var resource_status: Label = $HUD/ResourcePanel/Margin/ResourceStatus

var selected_resource := 0
var resource_status_elapsed := 0.0
var interaction_message := ""

func _ready() -> void:
    super._ready()
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        resource_status.text = "Resource error: SurvivalSimulationNode missing"
        return
    survival_sim.initialize_survival(42)
    if !survival_sim.get_last_error().is_empty():
        resource_status.text = "Resource error: %s" % survival_sim.get_last_error()
        return
    terrain_revision = sim.get_terrain_revision()
    surface_revision = sim.get_surface_revision()
    pending_surface_revision = surface_revision
    _update_resource_status()

func _process(delta: float) -> void:
    super._process(delta)
    resource_status_elapsed += delta
    if resource_status_elapsed >= 0.25:
        resource_status_elapsed = 0.0
        _update_resource_status()

func _unhandled_input(event: InputEvent) -> void:
    if event is InputEventKey and event.pressed and !event.echo:
        if event.keycode == KEY_Q:
            selected_resource = (selected_resource + 1) % RESOURCE_KEYS.size()
            interaction_message = ""
            _update_resource_status()
            get_viewport().set_input_as_handled()
            return
        if event.keycode == KEY_E and !survey_flight_enabled:
            _gather_selected_resource()
            get_viewport().set_input_as_handled()
            return
        if event.keycode == KEY_C and !survey_flight_enabled:
            _craft_stone_axe()
            get_viewport().set_input_as_handled()
            return
        if event.keycode == KEY_B and !survey_flight_enabled:
            _build_basic_shelter()
            get_viewport().set_input_as_handled()
            return
        if event.keycode == KEY_G and !survey_flight_enabled:
            _build_campfire()
            get_viewport().set_input_as_handled()
            return
        if event.keycode == KEY_R and !survey_flight_enabled:
            _add_campfire_fuel()
            get_viewport().set_input_as_handled()
            return
        if event.keycode == KEY_T and !survey_flight_enabled:
            _toggle_campfire()
            get_viewport().set_input_as_handled()
            return
    super._unhandled_input(event)

func _player_projected_position() -> Vector2:
    return Vector2(
        origin_east_m + float(player.position.x),
        origin_north_m - float(player.position.z)
    )

func _gather_selected_resource() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = "Resource simulation unavailable"
        return
    var position := _player_projected_position()
    var key: String = RESOURCE_KEYS[selected_resource]
    var realized := survival_sim.gather_resource_at(position.x, position.y, key)
    if realized > 0.0:
        interaction_message = "Gathered %.2f %s %s" % [
            realized,
            RESOURCE_UNITS[selected_resource],
            RESOURCE_NAMES[selected_resource],
        ]
    else:
        interaction_message = survival_sim.get_last_error()
    _update_resource_status()

func _craft_stone_axe() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = "Resource simulation unavailable"
        return
    if survival_sim.craft_stone_axe():
        interaction_message = "Crafted stone axe"
    else:
        interaction_message = survival_sim.get_last_error()
    _update_resource_status()

func _build_basic_shelter() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = "Resource simulation unavailable"
        return
    var position := _player_projected_position()
    if survival_sim.build_basic_shelter_at(position.x, position.y):
        interaction_message = "Built basic shelter"
    else:
        interaction_message = survival_sim.get_last_error()
    _update_resource_status()

func _build_campfire() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = "Resource simulation unavailable"
        return
    var position := _player_projected_position()
    if survival_sim.build_campfire_at(position.x, position.y):
        interaction_message = "Built campfire"
    else:
        interaction_message = survival_sim.get_last_error()
    _update_resource_status()

func _add_campfire_fuel() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = "Resource simulation unavailable"
        return
    var position := _player_projected_position()
    if survival_sim.add_campfire_fuel_at(position.x, position.y, 1.0):
        interaction_message = "Added 1 kg wood to campfire"
    else:
        interaction_message = survival_sim.get_last_error()
    _update_resource_status()

func _toggle_campfire() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = "Resource simulation unavailable"
        return
    var position := _player_projected_position()
    var campsite := survival_sim.get_campsite(position.x, position.y)
    if campsite.is_empty():
        interaction_message = survival_sim.get_last_error()
        return
    var target_lit := !bool(campsite.get("campfire_lit", false))
    if survival_sim.set_campfire_lit_at(position.x, position.y, target_lit):
        interaction_message = "Campfire lit" if target_lit else "Campfire extinguished"
    else:
        interaction_message = survival_sim.get_last_error()
    _update_resource_status()

func _update_resource_status() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        return
    var position := _player_projected_position()
    var local := survival_sim.get_local_resources(position.x, position.y)
    var inventory := survival_sim.get_inventory()
    var campsite := survival_sim.get_campsite(position.x, position.y)
    if local.is_empty() or inventory.is_empty() or campsite.is_empty():
        resource_status.text = "Resource error: %s" % survival_sim.get_last_error()
        return

    var key: String = RESOURCE_KEYS[selected_resource]
    var unit: String = RESOURCE_UNITS[selected_resource]
    var axe_text := "owned" if survival_sim.has_stone_axe() else "not crafted"
    var shelter_text := "built" if bool(campsite.get("shelter", false)) else "missing"
    var fire_text := "missing"
    if bool(campsite.get("campfire", false)):
        fire_text = "lit" if bool(campsite.get("campfire_lit", false)) else "unlit"
    var suitable_text := "yes" if bool(campsite.get("suitable", false)) else "no"

    var line := "%s  local %.2f %s  carried %.2f %s  [Q] select  [E] gather" % [
        RESOURCE_NAMES[selected_resource],
        float(local.get(key, 0.0)),
        unit,
        float(inventory.get(key, 0.0)),
        unit,
    ]
    line += "\nStone axe: %s  [C] craft (1 kg wood + 1 kg stone)" % axe_text
    line += "\nSite suitable: %s  Shelter: %s [B]  Campfire: %s fuel %.2f kg [G/R/T]" % [
        suitable_text,
        shelter_text,
        fire_text,
        float(campsite.get("campfire_fuel_kg", 0.0)),
    ]
    if !interaction_message.is_empty():
        line += "\n" + interaction_message
    resource_status.text = line
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
            _collect_selected_resource()
            get_viewport().set_input_as_handled()
            return
    super._unhandled_input(event)

func _collect_selected_resource() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        interaction_message = "Resource simulation unavailable"
        return
    var east_m := origin_east_m + float(player.position.x)
    var north_m := origin_north_m - float(player.position.z)
    var key: String = RESOURCE_KEYS[selected_resource]
    if survival_sim.collect_resource_at(east_m, north_m, key, 1.0):
        interaction_message = "Collected 1 %s %s" % [
            RESOURCE_UNITS[selected_resource],
            RESOURCE_NAMES[selected_resource],
        ]
    else:
        interaction_message = survival_sim.get_last_error()
    _update_resource_status()

func _update_resource_status() -> void:
    var survival_sim := sim as SurvivalSimulationNode
    if survival_sim == null:
        return
    var east_m := origin_east_m + float(player.position.x)
    var north_m := origin_north_m - float(player.position.z)
    var local := survival_sim.get_local_resources(east_m, north_m)
    var inventory := survival_sim.get_inventory()
    if local.is_empty() or inventory.is_empty():
        resource_status.text = "Resource error: %s" % survival_sim.get_last_error()
        return

    var key: String = RESOURCE_KEYS[selected_resource]
    var unit: String = RESOURCE_UNITS[selected_resource]
    var line := "%s  local %.2f %s  carried %.2f %s  [Q] select  [E] collect 1" % [
        RESOURCE_NAMES[selected_resource],
        float(local.get(key, 0.0)),
        unit,
        float(inventory.get(key, 0.0)),
        unit,
    ]
    if !interaction_message.is_empty():
        line += "\n" + interaction_message
    resource_status.text = line

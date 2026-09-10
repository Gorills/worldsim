extends SceneTree

const RESOURCE_KEYS := [
    "fresh_water",
    "plant_food",
    "wood",
    "stone",
    "metal_ore",
]
const ACTION_GATHER_BASE := 100
const ACTION_CRAFT_STONE_AXE := 200

func _initialize() -> void:
    TranslationServer.set_locale("ru")
    var packed := load("res://main.tscn")
    var scene: Node = packed.instantiate()
    root.add_child(scene)

func _process(_delta: float) -> bool:
    if Engine.get_process_frames() < 2:
        return false

    var viewer := root.get_node_or_null("WorldViewer")
    if viewer == null:
        return _fail(1, "WorldViewer is missing")
    var sim := viewer.get_node_or_null("Simulation") as SurvivalSimulationNode
    var resource_status := viewer.get_node_or_null("HUD/ResourcePanel/Margin/ResourceStatus") as Label
    if sim == null or resource_status == null:
        return _fail(2, "survival viewer dependencies are missing")

    if !InputMap.has_action("survival_interact"):
        return _fail(3, "survival_interact InputMap action is missing")
    var has_e_binding := false
    for event in InputMap.action_get_events("survival_interact"):
        if event is InputEventKey and event.physical_keycode == KEY_E:
            has_e_binding = true
            break
    if !has_e_binding:
        return _fail(4, "survival_interact is not bound to E")

    viewer.call("_update_resource_status")
    if !resource_status.text.contains("Региональный запас"):
        return _fail(5, "survival HUD did not load Russian localization")
    for obsolete_hotkey in ["[Q]", "[C]", "[B]", "[G/R/T]"]:
        if resource_status.text.contains(obsolete_hotkey):
            return _fail(6, "legacy per-action hotkey remains in survival HUD: %s" % obsolete_hotkey)
    if !resource_status.text.contains("[E] Действия"):
        return _fail(7, "unified interaction hint is missing")

    var localized_error := String(viewer.call(
        "_localized_error",
        "insufficient materials for stone axe"
    ))
    if localized_error != "Недостаточно материалов для каменного топора":
        return _fail(8, "known gameplay rejection was not localized")

    viewer.call("_rebuild_action_menu")
    var menu := viewer.get_node_or_null("SurvivalActions") as PopupMenu
    if menu == null or menu.item_count < 10:
        return _fail(9, "context action menu is missing expected actions")
    var craft_index := _menu_index_for_id(menu, ACTION_CRAFT_STONE_AXE)
    if craft_index < 0 or !menu.is_item_disabled(craft_index):
        return _fail(10, "stone axe action should be disabled with empty inventory")

    var position: Vector2 = viewer.call("_player_projected_position")
    var local := sim.get_local_resources(position.x, position.y)
    var gather_index := -1
    for index in range(RESOURCE_KEYS.size()):
        if float(local.get(RESOURCE_KEYS[index], 0.0)) > 0.0:
            gather_index = index
            break
    if gather_index < 0:
        return _fail(11, "spawn has no gatherable resource for interaction regression")

    var key: String = RESOURCE_KEYS[gather_index]
    var before_inventory := sim.get_inventory()
    var before_amount := float(before_inventory.get(key, 0.0))
    viewer.call("_execute_action", ACTION_GATHER_BASE + gather_index)
    var after_amount := float(sim.get_inventory().get(key, 0.0))
    if !(after_amount > before_amount):
        return _fail(12, "context gather action did not mutate authoritative inventory")
    if !String(viewer.get("interaction_message")).contains("Добыто"):
        return _fail(13, "successful gather feedback was not localized")

    # The diagnostic viewer used to advance one simulated hour every 0.25 real
    # seconds. Survival must keep the world stationary during an ordinary second
    # and advance exactly once when its 60-second gameplay interval closes.
    viewer.set("sim_focus_elapsed", 0.0)
    var tick_before := int(sim.get_tick())
    viewer.call("_process", 1.0)
    if int(sim.get_tick()) != tick_before:
        return _fail(14, "survival clock still advances at diagnostic-viewer speed")
    viewer.set("sim_focus_elapsed", 59.9)
    viewer.call("_process", 0.2)
    if int(sim.get_tick()) != tick_before + 1:
        return _fail(15, "survival clock did not advance at the 60-second interval")

    print("WORLDSIM_SURVIVAL_INTERACTION_OK resource=%s tick=%d" % [key, sim.get_tick()])
    quit(0)
    return true

func _menu_index_for_id(menu: PopupMenu, action_id: int) -> int:
    for index in range(menu.item_count):
        if !menu.is_item_separator(index) and menu.get_item_id(index) == action_id:
            return index
    return -1

func _fail(code: int, message: String) -> bool:
    push_error(message)
    quit(code)
    return true

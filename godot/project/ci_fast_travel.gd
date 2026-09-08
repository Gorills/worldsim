extends SceneTree

# Regression for the survey-flight contract: it must detach the walker from
# terrain collision, preserve large logical coordinates through origin shifting,
# stream the destination tile, and restore a safe grounded position.

func _initialize() -> void:
    var packed := load("res://main.tscn")
    var scene: Node = packed.instantiate()
    root.add_child(scene)

func _process(_delta: float) -> bool:
    if Engine.get_process_frames() < 2:
        return false

    var viewer := root.get_node_or_null("WorldViewer")
    if viewer == null:
        return _fail(1, "WorldViewer is missing")
    var player := viewer.get_node_or_null("Player") as CharacterBody3D
    var sim := viewer.get_node_or_null("Simulation")
    if player == null or sim == null:
        return _fail(2, "Fast-travel dependencies are missing")

    var minimap := viewer.get_node_or_null("HUD/MiniMapPanel/Margin/VBox/MapFrame/Inset/Layers/Map") as TextureRect
    var minimap_overlay := viewer.get_node_or_null("HUD/MiniMapPanel/Margin/VBox/MapFrame/Inset/Layers/Overlay")
    if minimap == null or minimap_overlay == null or minimap.texture == null:
        return _fail(15, "World minimap or its live overlay is missing")
    if minimap.texture.get_width() != 320 or minimap.texture.get_height() != 160:
        return _fail(16, "World minimap texture has the wrong dimensions")
    var initial_marker_uv: Vector2 = minimap_overlay.get("marker_uv")

    for action in ["toggle_survey_flight", "survey_descend", "survey_boost"]:
        if !InputMap.has_action(action):
            return _fail(3, "InputMap action is missing: %s" % action)
        if InputMap.action_get_events(action).is_empty():
            return _fail(3, "InputMap action has no binding: %s" % action)

    viewer.call("_set_survey_flight_enabled", true)
    if !bool(viewer.get("survey_flight_enabled")):
        return _fail(4, "Survey flight did not enable")
    if player.motion_mode != CharacterBody3D.MOTION_MODE_FLOATING or player.collision_mask != 0:
        return _fail(5, "Survey flight did not disable grounded collision")

    var initial_speed := float(viewer.call("_survey_speed_m_s"))
    viewer.call("_cycle_survey_speed", 1)
    if float(viewer.call("_survey_speed_m_s")) <= initial_speed:
        return _fail(6, "Survey speed did not increase")

    var before_flight := Vector2(
        float(viewer.get("origin_east_m")),
        float(viewer.get("origin_north_m"))
    )
    viewer.call("_move_survey_flight", Vector2(0.0, -1.0), 1.0)
    viewer.call("_update_minimap_marker")
    var after_flight := Vector2(
        float(viewer.get("origin_east_m")),
        float(viewer.get("origin_north_m"))
    )
    if before_flight.distance_to(after_flight) < 1000.0:
        return _fail(7, "Survey flight did not advance logical world coordinates")
    if absf(player.position.x) > 0.01 or absf(player.position.z) > 0.01:
        return _fail(8, "Survey flight leaked large coordinates into the scene tree")
    var moved_marker_uv: Vector2 = minimap_overlay.get("marker_uv")
    var heading_uv: Vector2 = minimap_overlay.get("heading_uv_delta")
    if initial_marker_uv.distance_to(moved_marker_uv) < 0.00001:
        return _fail(17, "Minimap marker did not follow the player")
    if heading_uv.is_zero_approx():
        return _fail(18, "Minimap heading arrow has no direction")

    # Five million meters catches regressions where fast travel writes the large
    # coordinate directly into the single-precision scene tree.
    player.position.x = 5000000.0 - float(viewer.get("origin_east_m"))
    viewer.call("_maybe_shift_origin")
    viewer.call("_refresh_streaming_center")
    if absf(float(viewer.get("origin_east_m")) - 5000000.0) > 0.01:
        return _fail(9, "Logical east coordinate was not preserved")
    if absf(player.position.x) > 0.01:
        return _fail(10, "Player was not recentered after long-distance travel")

    var current_chunk: Vector2i = viewer.get("current_chunk")
    var chunks: Dictionary = viewer.get("chunks")
    if !chunks.has(current_chunk):
        return _fail(11, "Destination terrain tile was not streamed synchronously")

    viewer.call("_set_survey_flight_enabled", false)
    if bool(viewer.get("survey_flight_enabled")):
        return _fail(12, "Survey flight did not disable")
    if player.motion_mode != CharacterBody3D.MOTION_MODE_GROUNDED or player.collision_mask == 0:
        return _fail(13, "Grounded collision was not restored")

    var east_m := float(viewer.get("origin_east_m")) + float(player.position.x)
    var north_m := float(viewer.get("origin_north_m")) + float(player.position.z)
    var expected_y := (
        float(sim.call("sample_terrain_height", east_m, north_m))
        - float(viewer.get("origin_height_m"))
        + 1.25
    )
    if absf(player.position.y - expected_y) > 0.01:
        return _fail(14, "Walker did not return to the destination surface")

    print("WORLDSIM_FAST_TRAVEL_OK east_km=%.1f destination_chunk=%s" % [east_m / 1000.0, current_chunk])
    quit(0)
    return true

func _fail(code: int, message: String) -> bool:
    push_error(message)
    quit(code)
    return true

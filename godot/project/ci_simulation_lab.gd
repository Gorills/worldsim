extends SceneTree

var scene: Node

func _initialize() -> void:
    var packed := load("res://simulation_lab.tscn")
    scene = packed.instantiate()
    root.add_child(scene)

func _process(_delta: float) -> bool:
    if Engine.get_process_frames() < 3:
        return false

    var sim := root.get_node_or_null("SimulationLab/Simulation") as WorldSimulationNode
    var selector := root.get_node_or_null(
        "SimulationLab/Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapHeading/Field"
    ) as OptionButton
    var map_view := root.get_node_or_null(
        "SimulationLab/Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapFrame/MapFrameMargin/MapAspect/MapStack/Map"
    ) as TextureRect
    var layer_key := root.get_node_or_null(
        "SimulationLab/Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapHeading/HeadingText/LayerKey"
    ) as Label
    var mean_value := root.get_node_or_null(
        "SimulationLab/Margin/VBox/Body/SidePanel/SideMargin/Side/Metrics/MeanCard/Margin/VBox/Value"
    ) as Label
    var inspector := root.get_node_or_null(
        "SimulationLab/Margin/VBox/Body/SidePanel/SideMargin/Side/Inspector"
    ) as Tree
    var step_button := root.get_node_or_null(
        "SimulationLab/Margin/VBox/HeaderPanel/HeaderMargin/Header/Step"
    ) as Button
    var chart := root.get_node_or_null(
        "SimulationLab/Margin/VBox/Body/SidePanel/SideMargin/Side/HistoryPanel/History"
    ) as Control
    var advanced := root.get_node_or_null(
        "SimulationLab/Margin/VBox/ModesPanel/ModesMargin/Modes/Advanced"
    ) as CheckButton
    if (
        sim == null or selector == null or map_view == null or layer_key == null or
        mean_value == null or inspector == null or step_button == null or
        chart == null or advanced == null
    ):
        push_error("Simulation laboratory controls are missing")
        quit(1)
        return true
    if !sim.get_last_error().is_empty() or sim.get_tick() != 0:
        push_error("Simulation laboratory did not initialize a fresh full world")
        quit(2)
        return true
    if map_view.texture == null:
        push_error("Simulation laboratory did not render its initial field map")
        quit(3)
        return true
    var image := map_view.texture.get_image()
    if image == null or image.get_width() != 512 or image.get_height() != 256:
        push_error("Simulation laboratory map texture has the wrong dimensions")
        quit(4)
        return true

    if (
        int(ProjectSettings.get_setting("display/window/size/viewport_width")) != 1920 or
        int(ProjectSettings.get_setting("display/window/size/viewport_height")) != 1080 or
        int(ProjectSettings.get_setting("display/window/size/mode")) != 3
    ):
        push_error("Simulation laboratory window defaults are not 1920x1080 fullscreen")
        quit(5)
        return true

    if selector.item_count != 4:
        push_error("Normal simulation laboratory mode is not a curated field set")
        quit(6)
        return true

    advanced.button_pressed = true
    var climate_index := -1
    var has_relative_humidity := false
    var has_hydrology := false
    var has_ecology := false
    var has_fire := false
    var has_soil_carbon := false
    var has_snow_albedo := false
    for index in range(selector.item_count):
        var metadata = selector.get_item_metadata(index)
        if !(metadata is Dictionary):
            continue
        var key := String((metadata as Dictionary).get("key", ""))
        has_hydrology = has_hydrology or key == "hydrology.surface_water_m3"
        has_ecology = has_ecology or key == "ecology.vegetation_carbon_kg"
        has_fire = has_fire or key == "ecology.fire_danger"
        has_soil_carbon = has_soil_carbon or key == "ecology.soil_carbon_kg"
        has_snow_albedo = has_snow_albedo or key == "climate.surface_albedo"
        has_relative_humidity = has_relative_humidity or key == "climate.relative_humidity"
        if key == "climate.surface_temperature_k":
            climate_index = index
    if !has_hydrology or !has_ecology or !has_fire or !has_soil_carbon or !has_snow_albedo or !has_relative_humidity or climate_index < 0:
        push_error("Advanced simulation laboratory mode does not expose all fields")
        quit(7)
        return true

    advanced.button_pressed = false
    scene.call("_apply_preset", 1, true)
    climate_index = -1
    has_relative_humidity = false
    has_snow_albedo = false
    for index in range(selector.item_count):
        var metadata = selector.get_item_metadata(index)
        if metadata is Dictionary:
            var key := String((metadata as Dictionary).get("key", ""))
            if key == "climate.surface_temperature_k":
                climate_index = index
            has_relative_humidity = has_relative_humidity or key == "climate.relative_humidity"
            has_snow_albedo = has_snow_albedo or key == "climate.surface_albedo"
    if climate_index < 0 or !has_relative_humidity or !has_snow_albedo:
        push_error("Climate diagnostic mode lacks coupled climate indicators")
        quit(8)
        return true
    selector.select(climate_index)
    selector.item_selected.emit(climate_index)
    if !layer_key.text.contains("climate.surface_temperature_k") or mean_value.text.is_empty():
        push_error("Simulation laboratory did not update selected-field statistics")
        quit(9)
        return true

    scene.call("_inspect_map_uv", Vector2(0.5, 0.5))
    var inspector_root := inspector.get_root()
    if (
        inspector_root == null or inspector_root.get_child_count() < 4 or
        inspector_root.get_child_count() > 6
    ):
        push_error("Simulation laboratory inspector is not scoped to the selected question")
        quit(10)
        return true

    step_button.pressed.emit()
    if sim.get_tick() != 24:
        push_error("Simulation laboratory day step did not advance 24 ticks")
        quit(11)
        return true
    if int(chart.call("get_sample_count")) < 2:
        push_error("Simulation laboratory did not append selected-field history")
        quit(12)
        return true

    scene.call("_advance_simulation", 720)
    if sim.get_tick() != 48 or int(scene.get("_pending_step_hours")) != 696:
        push_error("Simulation laboratory did not split a long advance across frames")
        quit(13)
        return true

    print("WORLDSIM_SIMULATION_LAB_OK fields=%d map=%dx%d tick=%d history=%d" % [
        sim.get_field_descriptors().size(),
        image.get_width(),
        image.get_height(),
        sim.get_tick(),
        int(chart.call("get_sample_count")),
    ])
    quit(0)
    return true

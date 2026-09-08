extends Control

const MAP_WIDTH := 512
const MAP_HEIGHT := 256
const PLAY_INTERVAL_SECONDS := 0.45
const MAX_HISTORY_SAMPLES := 240
const STEP_HOURS := [1, 24, 720]
const LOD_PALETTE := [
    Color(0.10, 0.18, 0.30),
    Color(0.13, 0.34, 0.52),
    Color(0.18, 0.55, 0.58),
    Color(0.50, 0.72, 0.42),
    Color(0.92, 0.72, 0.24),
    Color(0.91, 0.38, 0.20),
]

const FIELD_LABELS := {
    "core.active_level": "LAB_FIELD_ACTIVE_LEVEL",
    "geography.elevation_m": "LAB_FIELD_ELEVATION",
    "geography.land_fraction": "LAB_FIELD_LAND",
    "geology.continental_fraction": "LAB_FIELD_CONTINENTAL",
    "climate.surface_temperature_k": "LAB_FIELD_TEMPERATURE",
    "climate.precipitation_mm_day": "LAB_FIELD_PRECIPITATION",
    "climate.solar_flux_w_m2": "LAB_FIELD_SOLAR",
    "climate.weather_anomaly_k": "LAB_FIELD_WEATHER_ANOMALY",
    "hydrology.surface_depth_m": "LAB_FIELD_WATER_DEPTH",
    "hydrology.river_discharge_m3_day": "LAB_FIELD_RIVER_DISCHARGE",
    "hydrology.soil_water_m3": "LAB_FIELD_SOIL_WATER",
    "hydrology.groundwater_m3": "LAB_FIELD_GROUNDWATER",
    "hydrology.snow_water_m3": "LAB_FIELD_SNOW",
    "hydrology.flooded_fraction": "LAB_FIELD_FLOODED",
    "ecology.vegetation_carbon_kg": "LAB_FIELD_VEGETATION",
    "ecology.npp_kg_day": "LAB_FIELD_NPP",
    "ecology.soil_fertility": "LAB_FIELD_FERTILITY",
    "ecology.grass_carbon_kg": "LAB_FIELD_GRASS",
    "ecology.tree_carbon_kg": "LAB_FIELD_TREES",
}

const PRESETS := [
    {
        "id": "overview",
        "button": "Overview",
        "primary": "geography.elevation_m",
        "fields": [
            "geography.elevation_m",
            "geography.land_fraction",
            "geology.continental_fraction",
            "ecology.vegetation_carbon_kg",
        ],
        "inspect": [
            "geography.elevation_m",
            "geography.land_fraction",
            "climate.surface_temperature_k",
            "hydrology.surface_depth_m",
            "ecology.vegetation_carbon_kg",
        ],
        "purpose": "LAB_PURPOSE_OVERVIEW",
        "description": "LAB_DESCRIPTION_OVERVIEW",
    },
    {
        "id": "climate",
        "button": "Climate",
        "primary": "climate.surface_temperature_k",
        "fields": [
            "climate.surface_temperature_k",
            "climate.precipitation_mm_day",
            "climate.solar_flux_w_m2",
            "climate.weather_anomaly_k",
        ],
        "inspect": [
            "climate.surface_temperature_k",
            "climate.precipitation_mm_day",
            "climate.solar_flux_w_m2",
            "climate.weather_anomaly_k",
            "geography.elevation_m",
        ],
        "purpose": "LAB_PURPOSE_CLIMATE",
        "description": "LAB_DESCRIPTION_CLIMATE",
    },
    {
        "id": "water",
        "button": "Water",
        "primary": "hydrology.surface_depth_m",
        "fields": [
            "hydrology.surface_depth_m",
            "hydrology.river_discharge_m3_day",
            "hydrology.soil_water_m3",
            "hydrology.groundwater_m3",
            "hydrology.snow_water_m3",
            "hydrology.flooded_fraction",
        ],
        "inspect": [
            "climate.precipitation_mm_day",
            "hydrology.surface_depth_m",
            "hydrology.river_discharge_m3_day",
            "hydrology.soil_water_m3",
            "hydrology.groundwater_m3",
            "hydrology.flooded_fraction",
        ],
        "purpose": "LAB_PURPOSE_WATER",
        "description": "LAB_DESCRIPTION_WATER",
    },
    {
        "id": "ecosystem",
        "button": "Ecosystem",
        "primary": "ecology.vegetation_carbon_kg",
        "fields": [
            "ecology.vegetation_carbon_kg",
            "ecology.npp_kg_day",
            "ecology.soil_fertility",
            "ecology.grass_carbon_kg",
            "ecology.tree_carbon_kg",
        ],
        "inspect": [
            "ecology.vegetation_carbon_kg",
            "ecology.npp_kg_day",
            "ecology.soil_fertility",
            "ecology.grass_carbon_kg",
            "ecology.tree_carbon_kg",
            "climate.surface_temperature_k",
        ],
        "purpose": "LAB_PURPOSE_ECOSYSTEM",
        "description": "LAB_DESCRIPTION_ECOSYSTEM",
    },
    {
        "id": "lod",
        "button": "Lod",
        "primary": "core.active_level",
        "fields": [
            "core.active_level",
            "geography.elevation_m",
            "geography.land_fraction",
        ],
        "inspect": [
            "core.active_level",
            "geography.elevation_m",
            "geography.land_fraction",
            "hydrology.surface_water_m3",
            "ecology.vegetation_carbon_kg",
        ],
        "purpose": "LAB_PURPOSE_LOD",
        "description": "LAB_DESCRIPTION_LOD",
    },
]

@onready var sim: WorldSimulationNode = $Simulation
@onready var seed_input: SpinBox = $Margin/VBox/HeaderPanel/HeaderMargin/Header/Seed
@onready var reset_button: Button = $Margin/VBox/HeaderPanel/HeaderMargin/Header/Reset
@onready var play_button: Button = $Margin/VBox/HeaderPanel/HeaderMargin/Header/Play
@onready var step_button: Button = $Margin/VBox/HeaderPanel/HeaderMargin/Header/Step
@onready var step_size: OptionButton = $Margin/VBox/HeaderPanel/HeaderMargin/Header/StepSize
@onready var tick_label: Label = $Margin/VBox/HeaderPanel/HeaderMargin/Header/Time/Tick
@onready var advanced_toggle: CheckButton = $Margin/VBox/ModesPanel/ModesMargin/Modes/Advanced
@onready var field_selector: OptionButton = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapHeading/Field
@onready var reset_range_button: Button = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapHeading/ResetRange
@onready var layer_title: Label = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapHeading/HeadingText/LayerTitle
@onready var layer_key: Label = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapHeading/HeadingText/LayerKey
@onready var map_description: Label = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapDescription
@onready var map_view: TextureRect = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapFrame/MapFrameMargin/MapAspect/MapStack/Map
@onready var map_overlay: Control = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/MapFrame/MapFrameMargin/MapAspect/MapStack/Overlay
@onready var legend_view: TextureRect = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/Legend/Gradient
@onready var legend_min: Label = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/Legend/Minimum
@onready var legend_max: Label = $Margin/VBox/Body/MapPanel/MapMargin/MapContent/Legend/Maximum
@onready var question_text: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/QuestionText
@onready var mean_value: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/Metrics/MeanCard/Margin/VBox/Value
@onready var range_value: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/Metrics/RangeCard/Margin/VBox/Value
@onready var percentile_value: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/Metrics/PercentileCard/Margin/VBox/Value
@onready var coverage_value: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/Metrics/CoverageCard/Margin/VBox/Value
@onready var coverage_caption: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/Metrics/CoverageCard/Margin/VBox/Caption
@onready var history_title: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/HistoryHeader/HistoryTitle
@onready var history_summary: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/HistorySummary
@onready var history_chart: Control = $Margin/VBox/Body/SidePanel/SideMargin/Side/HistoryPanel/History
@onready var inspector_header: Label = $Margin/VBox/Body/SidePanel/SideMargin/Side/InspectorHeader
@onready var inspector_tree: Tree = $Margin/VBox/Body/SidePanel/SideMargin/Side/Inspector
@onready var focus_button: Button = $Margin/VBox/Body/SidePanel/SideMargin/Side/InspectorHeaderRow/Focus
@onready var clear_focus_button: Button = $Margin/VBox/Body/SidePanel/SideMargin/Side/InspectorHeaderRow/ClearFocus
@onready var status: Label = $Margin/VBox/StatusPanel/StatusMargin/Status

var _descriptors: Array = []
var _scale_ranges: Dictionary = {}
var _history: Array[Vector2] = []
var _selected_direction := Vector3(1.0, 0.0, 0.0)
var _selected_uv := Vector2(0.5, 0.5)
var _preset_index := 0
var _play_elapsed := 0.0
var _busy := false
var _focused := false
var _mode_buttons: Array[Button] = []

func _ready() -> void:
    reset_button.pressed.connect(_reset_world)
    play_button.toggled.connect(_on_play_toggled)
    step_button.pressed.connect(_on_step_pressed)
    field_selector.item_selected.connect(_on_field_selected)
    reset_range_button.pressed.connect(_on_reset_range)
    map_view.gui_input.connect(_on_map_gui_input)
    focus_button.pressed.connect(_on_focus_pressed)
    clear_focus_button.pressed.connect(_on_clear_focus_pressed)
    advanced_toggle.toggled.connect(_on_advanced_toggled)

    var group := ButtonGroup.new()
    group.allow_unpress = false
    for index in range(PRESETS.size()):
        var button := get_node(
            "Margin/VBox/ModesPanel/ModesMargin/Modes/%s" % String(PRESETS[index]["button"])
        ) as Button
        button.button_group = group
        button.pressed.connect(_on_preset_pressed.bind(index))
        _mode_buttons.push_back(button)
    _mode_buttons[0].button_pressed = true

    step_size.clear()
    step_size.add_item(tr("LAB_STEP_HOUR"))
    step_size.add_item(tr("LAB_STEP_DAY"))
    step_size.add_item(tr("LAB_STEP_MONTH"))
    step_size.select(1)

    inspector_tree.columns = 3
    inspector_tree.column_titles_visible = true
    inspector_tree.set_column_title(0, tr("LAB_FIELD_COLUMN"))
    inspector_tree.set_column_title(1, tr("LAB_VALUE_COLUMN"))
    inspector_tree.set_column_title(2, tr("LAB_UNIT_COLUMN"))
    inspector_tree.set_column_expand(0, true)
    inspector_tree.set_column_expand(1, false)
    inspector_tree.set_column_custom_minimum_width(1, 112)
    inspector_tree.set_column_expand(2, false)
    inspector_tree.set_column_custom_minimum_width(2, 84)
    _reset_world()

func _process(delta: float) -> void:
    if !play_button.button_pressed or _busy:
        return
    _play_elapsed += delta
    if _play_elapsed < PLAY_INTERVAL_SECONDS:
        return
    _play_elapsed = fmod(_play_elapsed, PLAY_INTERVAL_SECONDS)
    _advance_simulation(_selected_step_hours())

func _reset_world() -> void:
    if _busy:
        return
    play_button.button_pressed = false
    _set_busy(true)
    sim.initialize(int(seed_input.value))
    if !_check_sim_error():
        _set_busy(false)
        return

    _descriptors = sim.get_field_descriptors()
    if _descriptors.is_empty():
        _show_error("full simulation returned no field descriptors")
        _set_busy(false)
        return

    _scale_ranges.clear()
    _history.clear()
    _selected_direction = Vector3(1.0, 0.0, 0.0)
    _selected_uv = Vector2(0.5, 0.5)
    _focused = false
    _apply_preset(_preset_index, false)
    if !_refresh_visualization(true, true):
        _set_busy(false)
        return
    _inspect_direction(_selected_direction, _selected_uv)
    status.text = tr("LAB_STATUS_READY") % int(seed_input.value)
    _set_busy(false)

func _on_preset_pressed(index: int) -> void:
    if !_busy:
        _apply_preset(index, true)

func _apply_preset(index: int, refresh: bool) -> void:
    _preset_index = clampi(index, 0, PRESETS.size() - 1)
    _mode_buttons[_preset_index].button_pressed = true
    var preset: Dictionary = PRESETS[_preset_index]
    question_text.text = tr(String(preset["purpose"]))
    _populate_fields(String(preset["primary"]))
    _history.clear()
    if refresh:
        _refresh_visualization(false, true)
        _inspect_direction(_selected_direction, _selected_uv)
        status.text = tr("LAB_STATUS_MODE") % tr(_mode_translation_key())

func _on_advanced_toggled(_enabled: bool) -> void:
    if _busy:
        return
    var current_key := _current_key()
    _populate_fields(current_key)
    _history.clear()
    _refresh_visualization(false, true)
    _inspect_direction(_selected_direction, _selected_uv)

func _populate_fields(preferred_key: String) -> void:
    field_selector.clear()
    var keys: Array = []
    if advanced_toggle.button_pressed:
        for descriptor_variant in _descriptors:
            keys.push_back(String((descriptor_variant as Dictionary).get("key", "")))
        keys.push_back("core.active_level")
    else:
        keys = (PRESETS[_preset_index]["fields"] as Array).duplicate()

    var preferred_index := -1
    for key_variant in keys:
        var key := String(key_variant)
        var descriptor := _descriptor_by_key(key)
        if descriptor.is_empty():
            continue
        field_selector.add_item("%s  ·  %s" % [_field_label(key), _display_unit(descriptor)])
        var item_index := field_selector.item_count - 1
        field_selector.set_item_metadata(item_index, descriptor)
        field_selector.set_item_tooltip(item_index, key)
        if key == preferred_key:
            preferred_index = item_index
    if field_selector.item_count == 0:
        return
    field_selector.select(preferred_index if preferred_index >= 0 else 0)

func _descriptor_by_key(key: String) -> Dictionary:
    if key == "core.active_level":
        return {
            "key": key,
            "unit": tr("LAB_UNIT_LEVEL"),
            "semantics": "intensive",
            "kind": "lod",
        }
    for descriptor_variant in _descriptors:
        var descriptor := descriptor_variant as Dictionary
        if String(descriptor.get("key", "")) == key:
            return descriptor
    return {}

func _on_play_toggled(playing: bool) -> void:
    play_button.text = tr("LAB_PAUSE") if playing else tr("LAB_PLAY")
    _play_elapsed = 0.0

func _on_step_pressed() -> void:
    if !_busy:
        _advance_simulation(_selected_step_hours())

func _selected_step_hours() -> int:
    var index := clampi(step_size.selected, 0, STEP_HOURS.size() - 1)
    return STEP_HOURS[index]

func _advance_simulation(hours: int) -> void:
    _set_busy(true)
    sim.step_hours(hours)
    if !_check_sim_error():
        play_button.button_pressed = false
        _set_busy(false)
        return
    if !_refresh_visualization(false, true):
        play_button.button_pressed = false
        _set_busy(false)
        return
    _inspect_direction(_selected_direction, _selected_uv)
    status.text = tr("LAB_STATUS_ADVANCED") % hours
    _set_busy(false)

func _on_field_selected(_index: int) -> void:
    if _busy:
        return
    _history.clear()
    _refresh_visualization(false, true)
    _inspect_direction(_selected_direction, _selected_uv)

func _on_reset_range() -> void:
    if _busy:
        return
    _scale_ranges.erase(_current_key())
    _refresh_visualization(true, false)

func _on_focus_pressed() -> void:
    sim.set_focus_direction(_selected_direction)
    if _check_sim_error():
        _focused = true
        map_overlay.call("set_marker", _selected_uv, _focused)
        status.text = tr("LAB_STATUS_FOCUS_PENDING")

func _on_clear_focus_pressed() -> void:
    sim.clear_focus()
    if _check_sim_error():
        _focused = false
        map_overlay.call("set_marker", _selected_uv, _focused)
        status.text = tr("LAB_STATUS_CLEAR_PENDING")

func _on_map_gui_input(event: InputEvent) -> void:
    if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
        var click := event as InputEventMouseButton
        var uv := Vector2(
            click.position.x / maxf(map_view.size.x, 1.0),
            click.position.y / maxf(map_view.size.y, 1.0)
        )
        _inspect_map_uv(uv)
        map_view.accept_event()

func _inspect_map_uv(uv: Vector2) -> void:
    _selected_uv = Vector2(
        clampf(uv.x, 0.0, 0.999999),
        clampf(uv.y, 0.0, 0.999999)
    )
    var latitude := (0.5 - _selected_uv.y) * PI
    var longitude := (2.0 * _selected_uv.x - 1.0) * PI
    _selected_direction = Vector3(
        cos(latitude) * cos(longitude),
        cos(latitude) * sin(longitude),
        sin(latitude)
    )
    _focused = false
    _inspect_direction(_selected_direction, _selected_uv)

func _inspect_direction(direction: Vector3, uv: Vector2) -> void:
    var inspected: Dictionary = sim.inspect_direction(direction)
    if inspected.is_empty():
        _check_sim_error()
        return

    var latitude_deg := (0.5 - uv.y) * 180.0
    var longitude_deg := (2.0 * uv.x - 1.0) * 180.0
    inspector_header.text = tr("LAB_INSPECTED_CELL") % [
        latitude_deg,
        longitude_deg,
        int(inspected["level"]),
    ]
    map_overlay.call("set_marker", uv, _focused)

    inspector_tree.clear()
    var root := inspector_tree.create_item()
    var values: Dictionary = inspected["values"]
    var area := float(inspected["area_m2"])
    var inspect_keys: Array = (PRESETS[_preset_index]["inspect"] as Array).duplicate()
    var current_key := _current_key()
    if !inspect_keys.has(current_key):
        inspect_keys.push_front(current_key)
    for key_variant in inspect_keys:
        var key := String(key_variant)
        var descriptor := _descriptor_by_key(key)
        if descriptor.is_empty():
            continue
        var value := float(inspected["level"]) if key == "core.active_level" else float(values.get(key, 0.0))
        if String(descriptor.get("semantics", "intensive")) == "extensive":
            value /= area
        var item := inspector_tree.create_item(root)
        item.set_text(0, _field_label(key))
        item.set_tooltip_text(0, key)
        item.set_text(1, _format_number(value))
        item.set_text(2, _display_unit(descriptor))

func _refresh_visualization(reset_range: bool, record_history: bool) -> bool:
    var descriptor := _current_descriptor()
    if descriptor.is_empty():
        _show_error("no diagnostic field is selected")
        return false
    var key := String(descriptor["key"])
    var is_lod := String(descriptor.get("kind", "")) == "lod"
    var map_values := PackedFloat64Array()
    if is_lod:
        var lod_values := sim.sample_lod_equirectangular(MAP_WIDTH, MAP_HEIGHT)
        map_values.resize(lod_values.size())
        for i in range(lod_values.size()):
            map_values[i] = float(lod_values[i])
    else:
        map_values = sim.sample_field_equirectangular(key, MAP_WIDTH, MAP_HEIGHT, true)
    if !_check_sim_error():
        return false
    if map_values.size() != MAP_WIDTH * MAP_HEIGHT:
        _show_error("diagnostic map size mismatch")
        return false

    var field_stats := _calculate_active_statistics(descriptor)
    if field_stats.is_empty():
        return false
    var scale := _update_scale(key, field_stats, reset_range, is_lod)
    _render_map(map_values, float(scale["min"]), float(scale["max"]), key, is_lod)
    _update_layer_copy(descriptor)
    _update_metrics(descriptor, field_stats)
    _update_tick_label()

    if record_history:
        _history.push_back(Vector2(float(sim.get_tick()), float(field_stats["mean"])))
        if _history.size() > MAX_HISTORY_SAMPLES:
            _history.pop_front()
    history_title.text = tr("LAB_HISTORY") % _field_label(key)
    _update_history_summary(descriptor)
    history_chart.call("set_samples", _history)
    return true

func _calculate_active_statistics(descriptor: Dictionary) -> Dictionary:
    var packet: Dictionary = sim.get_render_packet()
    if packet.is_empty():
        _check_sim_error()
        return {}
    var areas: PackedFloat64Array = packet["areas_m2"]
    var key := String(descriptor["key"])
    var is_lod := String(descriptor.get("kind", "")) == "lod"
    var raw_values := PackedFloat64Array()
    if is_lod:
        var levels: PackedInt32Array = packet["levels"]
        raw_values.resize(levels.size())
        for i in range(levels.size()):
            raw_values[i] = float(levels[i])
    else:
        raw_values = sim.get_field_values(key)
    if raw_values.size() != areas.size() or raw_values.is_empty():
        _show_error("active field array is missing or misaligned")
        return {}

    var extensive := String(descriptor.get("semantics", "intensive")) == "extensive"
    var display_values := PackedFloat64Array()
    display_values.resize(raw_values.size())
    var minimum := INF
    var maximum := -INF
    var weighted_sum := 0.0
    var area_sum := 0.0
    var raw_total := 0.0
    for i in range(raw_values.size()):
        var area := float(areas[i])
        var raw := float(raw_values[i])
        var value := raw / area if extensive else raw
        if !is_finite(value) or !is_finite(area) or area <= 0.0:
            _show_error("non-finite diagnostic value")
            return {}
        display_values[i] = value
        minimum = minf(minimum, value)
        maximum = maxf(maximum, value)
        weighted_sum += value * area
        area_sum += area
        raw_total += raw

    var sorted := display_values.duplicate()
    sorted.sort()
    var last := sorted.size() - 1
    var p02 := float(sorted[clampi(int(floor(float(last) * 0.02)), 0, last)])
    var p98 := float(sorted[clampi(int(ceil(float(last) * 0.98)), 0, last)])
    return {
        "min": minimum,
        "max": maximum,
        "mean": weighted_sum / area_sum,
        "p02": p02,
        "p98": p98,
        "total": raw_total,
        "cell_count": raw_values.size(),
    }

func _update_scale(
    key: String,
    field_stats: Dictionary,
    reset_range: bool,
    is_lod: bool
) -> Dictionary:
    var lower := float(field_stats["p02"])
    var upper := float(field_stats["p98"])
    if is_lod:
        lower = 4.0
        upper = 7.0
    elif is_equal_approx(lower, upper):
        var padding := maxf(absf(lower) * 0.05, 1.0e-9)
        lower -= padding
        upper += padding

    if !reset_range and _scale_ranges.has(key):
        var previous: Dictionary = _scale_ranges[key]
        lower = minf(lower, float(previous["min"]))
        upper = maxf(upper, float(previous["max"]))
    var scale := {"min": lower, "max": upper}
    _scale_ranges[key] = scale
    return scale

func _render_map(
    values: PackedFloat64Array,
    lower: float,
    upper: float,
    key: String,
    is_lod: bool
) -> void:
    var pixels := PackedByteArray()
    pixels.resize(MAP_WIDTH * MAP_HEIGHT * 3)
    for i in range(values.size()):
        var color := _color_for_value(float(values[i]), lower, upper, key, is_lod)
        var byte_index := i * 3
        pixels[byte_index] = clampi(roundi(color.r * 255.0), 0, 255)
        pixels[byte_index + 1] = clampi(roundi(color.g * 255.0), 0, 255)
        pixels[byte_index + 2] = clampi(roundi(color.b * 255.0), 0, 255)
    var map_image := Image.create_from_data(
        MAP_WIDTH, MAP_HEIGHT, false, Image.FORMAT_RGB8, pixels
    )
    map_view.texture = ImageTexture.create_from_image(map_image)
    _render_legend(lower, upper, key, is_lod)

func _render_legend(lower: float, upper: float, key: String, is_lod: bool) -> void:
    var pixels := PackedByteArray()
    pixels.resize(256 * 3)
    for x in range(256):
        var value := lerpf(lower, upper, float(x) / 255.0)
        var color := _color_for_value(value, lower, upper, key, is_lod)
        pixels[x * 3] = clampi(roundi(color.r * 255.0), 0, 255)
        pixels[x * 3 + 1] = clampi(roundi(color.g * 255.0), 0, 255)
        pixels[x * 3 + 2] = clampi(roundi(color.b * 255.0), 0, 255)
    var legend_image := Image.create_from_data(256, 1, false, Image.FORMAT_RGB8, pixels)
    legend_view.texture = ImageTexture.create_from_image(legend_image)
    legend_min.text = "%s %s" % [_format_number(lower), _display_unit(_current_descriptor())]
    legend_max.text = "%s %s" % [_format_number(upper), _display_unit(_current_descriptor())]

func _color_for_value(
    value: float,
    lower: float,
    upper: float,
    key: String,
    is_lod: bool
) -> Color:
    if !is_finite(value):
        return Color(1.0, 0.0, 1.0)
    if is_lod:
        var palette_index := clampi(roundi(value) - 2, 0, LOD_PALETTE.size() - 1)
        return LOD_PALETTE[palette_index]
    if key == "geography.elevation_m":
        return _elevation_color(value)

    if lower < 0.0 and upper > 0.0:
        if value < 0.0:
            return Color(0.88, 0.89, 0.86).lerp(
                Color(0.10, 0.34, 0.70),
                clampf(value / lower, 0.0, 1.0)
            )
        return Color(0.88, 0.89, 0.86).lerp(
            Color(0.84, 0.18, 0.12),
            clampf(value / upper, 0.0, 1.0)
        )

    var t := clampf((value - lower) / maxf(upper - lower, 1.0e-30), 0.0, 1.0)
    if key.begins_with("hydrology.") or key == "climate.precipitation_mm_day":
        return _ramp3(t, Color(0.94, 0.96, 0.91), Color(0.18, 0.62, 0.78), Color(0.02, 0.10, 0.32))
    if key.begins_with("ecology."):
        return _ramp3(t, Color(0.16, 0.12, 0.08), Color(0.55, 0.68, 0.24), Color(0.05, 0.37, 0.15))
    if key == "climate.surface_temperature_k":
        return _ramp3(t, Color(0.10, 0.28, 0.65), Color(0.92, 0.86, 0.43), Color(0.82, 0.16, 0.10))
    if key.begins_with("magic."):
        return _ramp3(t, Color(0.08, 0.05, 0.16), Color(0.38, 0.20, 0.66), Color(0.92, 0.55, 1.0))
    return _ramp3(t, Color(0.06, 0.12, 0.28), Color(0.12, 0.68, 0.72), Color(0.96, 0.82, 0.25))

func _ramp3(t: float, low: Color, middle: Color, high: Color) -> Color:
    if t < 0.5:
        return low.lerp(middle, t * 2.0)
    return middle.lerp(high, (t - 0.5) * 2.0)

func _elevation_color(height_m: float) -> Color:
    if height_m < 0.0:
        return Color(0.16, 0.48, 0.70).lerp(
            Color(0.015, 0.045, 0.11),
            clampf(-height_m / 6000.0, 0.0, 1.0)
        )
    if height_m < 900.0:
        return Color(0.22, 0.48, 0.23).lerp(
            Color(0.50, 0.52, 0.27),
            clampf(height_m / 900.0, 0.0, 1.0)
        )
    if height_m < 3000.0:
        return Color(0.50, 0.52, 0.27).lerp(
            Color(0.48, 0.39, 0.31),
            clampf((height_m - 900.0) / 2100.0, 0.0, 1.0)
        )
    return Color(0.48, 0.39, 0.31).lerp(
        Color(0.92, 0.92, 0.90),
        clampf((height_m - 3000.0) / 3500.0, 0.0, 1.0)
    )

func _update_layer_copy(descriptor: Dictionary) -> void:
    var key := String(descriptor["key"])
    layer_title.text = _field_label(key)
    layer_key.text = "%s  ·  %s" % [key, _display_unit(descriptor)]
    var preset_fields: Array = PRESETS[_preset_index]["fields"]
    map_description.text = tr(String(PRESETS[_preset_index]["description"])) \
        if preset_fields.has(key) else tr("LAB_DESCRIPTION_ADVANCED")

func _update_metrics(descriptor: Dictionary, field_stats: Dictionary) -> void:
    var unit := _display_unit(descriptor)
    mean_value.text = "%s %s" % [_format_number(float(field_stats["mean"])), unit]
    range_value.text = "%s — %s" % [
        _format_number(float(field_stats["min"])),
        _format_number(float(field_stats["max"])),
    ]
    percentile_value.text = "%s — %s" % [
        _format_number(float(field_stats["p02"])),
        _format_number(float(field_stats["p98"])),
    ]
    if String(descriptor.get("semantics", "intensive")) == "extensive":
        coverage_value.text = "%s %s" % [
            _format_number(float(field_stats["total"])),
            String(descriptor["unit"]),
        ]
        coverage_caption.text = tr("LAB_METRIC_TOTAL")
    else:
        coverage_value.text = _format_integer(int(field_stats["cell_count"]))
        coverage_caption.text = tr("LAB_METRIC_CELLS")

func _update_history_summary(descriptor: Dictionary) -> void:
    if _history.size() < 2:
        history_summary.text = tr("LAB_HISTORY_INITIAL")
        return
    var delta := _history[_history.size() - 1].y - _history[0].y
    history_summary.text = tr("LAB_HISTORY_DELTA") % [
        "%+.4f" % delta,
        _display_unit(descriptor),
    ]

func _update_tick_label() -> void:
    var tick := sim.get_tick()
    tick_label.text = tr("LAB_TICK") % [tick, float(tick) / 24.0]

func _field_label(key: String) -> String:
    if FIELD_LABELS.has(key):
        return tr(String(FIELD_LABELS[key]))
    var parts := key.split(".")
    return String(parts[parts.size() - 1]).replace("_", " ").capitalize()

func _display_unit(descriptor: Dictionary) -> String:
    var unit := String(descriptor.get("unit", ""))
    if String(descriptor.get("semantics", "intensive")) == "extensive":
        return "%s/m²" % unit
    return unit

func _format_number(value: float) -> String:
    var absolute := absf(value)
    if absolute >= 1.0e7 or (absolute > 0.0 and absolute < 1.0e-3):
        return String.num_scientific(value)
    if absolute >= 1000.0:
        return "%.0f" % value
    if absolute >= 10.0:
        return "%.2f" % value
    return "%.4f" % value

func _format_integer(value: int) -> String:
    var digits := str(value)
    var formatted := ""
    for index in range(digits.length()):
        if index > 0 and (digits.length() - index) % 3 == 0:
            formatted += " "
        formatted += digits[index]
    return formatted

func _current_descriptor() -> Dictionary:
    if field_selector.item_count == 0 or field_selector.selected < 0:
        return {}
    return field_selector.get_item_metadata(field_selector.selected) as Dictionary

func _current_key() -> String:
    return String(_current_descriptor().get("key", ""))

func _mode_translation_key() -> String:
    return "LAB_MODE_%s" % String(PRESETS[_preset_index]["id"]).to_upper()

func _set_busy(busy: bool) -> void:
    _busy = busy
    seed_input.editable = !busy
    reset_button.disabled = busy
    step_button.disabled = busy
    step_size.disabled = busy
    field_selector.disabled = busy
    reset_range_button.disabled = busy
    focus_button.disabled = busy
    clear_focus_button.disabled = busy
    advanced_toggle.disabled = busy
    for button in _mode_buttons:
        button.disabled = busy

func _check_sim_error() -> bool:
    var error := sim.get_last_error()
    if error.is_empty():
        return true
    _show_error(error)
    return false

func _show_error(message: String) -> void:
    status.text = tr("HUD_STATUS_ERROR") % message

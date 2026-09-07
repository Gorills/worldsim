extends Control

const MAP_WIDTH := 1024
const MAP_HEIGHT := 512

const LAYER_ELEVATION := "elevation"
const LAYER_PLATES := "plates"
const LAYER_FORCING := "forcing"

const PLATE_COLORS := [
    Color(0.84, 0.31, 0.30),
    Color(0.30, 0.53, 0.84),
    Color(0.32, 0.70, 0.45),
    Color(0.83, 0.62, 0.24),
    Color(0.62, 0.40, 0.78),
    Color(0.20, 0.68, 0.70),
    Color(0.86, 0.43, 0.67),
    Color(0.55, 0.66, 0.24),
    Color(0.75, 0.43, 0.22),
    Color(0.35, 0.45, 0.72),
    Color(0.44, 0.72, 0.65),
    Color(0.73, 0.36, 0.45),
    Color(0.57, 0.50, 0.82),
    Color(0.26, 0.62, 0.35),
    Color(0.82, 0.70, 0.34),
    Color(0.31, 0.57, 0.62),
]

@onready var sim: WorldSimulationNode = $Simulation
@onready var seed_input: SpinBox = $Margin/VBox/Toolbar/Seed
@onready var generate_button: Button = $Margin/VBox/Toolbar/Generate
@onready var status: Label = $Margin/VBox/Toolbar/Status
@onready var elevation_button: Button = $Margin/VBox/LayerBar/Elevation
@onready var plates_button: Button = $Margin/VBox/LayerBar/Plates
@onready var forcing_button: Button = $Margin/VBox/LayerBar/Forcing
@onready var map_view: TextureRect = $Margin/VBox/MapPanel/Map

var _heights := PackedFloat32Array()
var _plate_ids := PackedInt32Array()
var _forcing := PackedFloat32Array()
var _current_layer := LAYER_ELEVATION
var _min_height := 0.0
var _max_height := 0.0

func _ready() -> void:
    generate_button.pressed.connect(_on_generate_pressed)
    elevation_button.pressed.connect(_show_elevation)
    plates_button.pressed.connect(_show_plates)
    forcing_button.pressed.connect(_show_forcing)
    _generate_map(int(seed_input.value))

func _on_generate_pressed() -> void:
    _generate_map(int(seed_input.value))

func _show_elevation() -> void:
    _set_layer(LAYER_ELEVATION)

func _show_plates() -> void:
    _set_layer(LAYER_PLATES)

func _show_forcing() -> void:
    _set_layer(LAYER_FORCING)

func _set_layer(layer: String) -> void:
    _current_layer = layer
    if !_heights.is_empty():
        _render_current_layer()
    _set_controls_enabled(true)

func _generate_map(seed: int) -> void:
    _set_controls_enabled(false)
    status.text = tr("MAP_STATUS_GENERATING") % seed

    sim.initialize_terrain_world(seed)
    if !sim.get_last_error().is_empty():
        _show_error(sim.get_last_error())
        return

    var heights := sim.sample_terrain_equirectangular(MAP_WIDTH, MAP_HEIGHT)
    if heights.size() != MAP_WIDTH * MAP_HEIGHT:
        _show_error(sim.get_last_error())
        return

    var tectonics: Dictionary = sim.sample_tectonics_equirectangular(MAP_WIDTH, MAP_HEIGHT)
    if !sim.get_last_error().is_empty():
        _show_error(sim.get_last_error())
        return
    if !tectonics.has("plate_id") or !tectonics.has("forcing"):
        _show_error("tectonic debug layers are missing")
        return

    var plate_ids: PackedInt32Array = tectonics["plate_id"]
    var forcing: PackedFloat32Array = tectonics["forcing"]
    if plate_ids.size() != MAP_WIDTH * MAP_HEIGHT or forcing.size() != MAP_WIDTH * MAP_HEIGHT:
        _show_error("tectonic debug layer size mismatch")
        return

    _heights = heights
    _plate_ids = plate_ids
    _forcing = forcing

    _min_height = float(_heights[0])
    _max_height = _min_height
    for height_variant in _heights:
        var height_m := float(height_variant)
        _min_height = minf(_min_height, height_m)
        _max_height = maxf(_max_height, height_m)

    _render_current_layer()
    status.text = tr("MAP_STATUS_READY") % [
        seed,
        MAP_WIDTH,
        MAP_HEIGHT,
        _min_height,
        _max_height
    ]
    _set_controls_enabled(true)

func _show_error(message: String) -> void:
    status.text = tr("HUD_STATUS_ERROR") % message
    _set_controls_enabled(true)

func _set_controls_enabled(enabled: bool) -> void:
    generate_button.disabled = !enabled
    elevation_button.disabled = !enabled or _current_layer == LAYER_ELEVATION
    plates_button.disabled = !enabled or _current_layer == LAYER_PLATES
    forcing_button.disabled = !enabled or _current_layer == LAYER_FORCING

func _render_current_layer() -> void:
    var pixels := PackedByteArray()
    pixels.resize(MAP_WIDTH * MAP_HEIGHT * 3)

    for i in range(MAP_WIDTH * MAP_HEIGHT):
        var color := Color.BLACK
        match _current_layer:
            LAYER_PLATES:
                color = _plate_color(int(_plate_ids[i]))
            LAYER_FORCING:
                color = _forcing_color(float(_forcing[i]))
            _:
                color = _elevation_color(float(_heights[i]))

        var byte_index := i * 3
        pixels[byte_index] = clampi(roundi(color.r * 255.0), 0, 255)
        pixels[byte_index + 1] = clampi(roundi(color.g * 255.0), 0, 255)
        pixels[byte_index + 2] = clampi(roundi(color.b * 255.0), 0, 255)

    var image := Image.create_from_data(
        MAP_WIDTH,
        MAP_HEIGHT,
        false,
        Image.FORMAT_RGB8,
        pixels
    )
    map_view.texture = ImageTexture.create_from_image(image)

func _plate_color(plate_id: int) -> Color:
    return PLATE_COLORS[plate_id % PLATE_COLORS.size()]

func _forcing_color(value: float) -> Color:
    var neutral := Color(0.11, 0.12, 0.14)
    if value > 0.0:
        return neutral.lerp(Color(0.92, 0.20, 0.12), clampf(value / 0.65, 0.0, 1.0))
    return neutral.lerp(Color(0.12, 0.34, 0.92), clampf(-value / 0.65, 0.0, 1.0))

func _elevation_color(height_m: float) -> Color:
    if height_m < 0.0:
        var ocean_t := clampf(-height_m / 6000.0, 0.0, 1.0)
        return Color(0.16, 0.48, 0.70).lerp(Color(0.015, 0.045, 0.11), ocean_t)

    if height_m < 900.0:
        var lowland_t := clampf(height_m / 900.0, 0.0, 1.0)
        return Color(0.22, 0.48, 0.23).lerp(Color(0.50, 0.52, 0.27), lowland_t)

    if height_m < 3000.0:
        var highland_t := clampf((height_m - 900.0) / 2100.0, 0.0, 1.0)
        return Color(0.50, 0.52, 0.27).lerp(Color(0.48, 0.39, 0.31), highland_t)

    var mountain_t := clampf((height_m - 3000.0) / 3500.0, 0.0, 1.0)
    return Color(0.48, 0.39, 0.31).lerp(Color(0.92, 0.92, 0.90), mountain_t)

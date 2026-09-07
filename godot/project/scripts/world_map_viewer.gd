extends Control

const MAP_WIDTH := 1024
const MAP_HEIGHT := 512

@onready var sim: WorldSimulationNode = $Simulation
@onready var seed_input: SpinBox = $Margin/VBox/Toolbar/Seed
@onready var generate_button: Button = $Margin/VBox/Toolbar/Generate
@onready var status: Label = $Margin/VBox/Toolbar/Status
@onready var map_view: TextureRect = $Margin/VBox/MapPanel/Map

func _ready() -> void:
    generate_button.pressed.connect(_on_generate_pressed)
    _generate_map(int(seed_input.value))

func _on_generate_pressed() -> void:
    _generate_map(int(seed_input.value))

func _generate_map(seed: int) -> void:
    generate_button.disabled = true
    status.text = tr("MAP_STATUS_GENERATING") % seed

    sim.initialize_terrain_world(seed)
    if !sim.get_last_error().is_empty():
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        generate_button.disabled = false
        return

    var heights := sim.sample_terrain_equirectangular(MAP_WIDTH, MAP_HEIGHT)
    if heights.size() != MAP_WIDTH * MAP_HEIGHT:
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        generate_button.disabled = false
        return

    var pixels := PackedByteArray()
    pixels.resize(MAP_WIDTH * MAP_HEIGHT * 3)

    var min_height := float(heights[0])
    var max_height := min_height
    for i in range(heights.size()):
        var height_m := float(heights[i])
        min_height = minf(min_height, height_m)
        max_height = maxf(max_height, height_m)
        var color := _elevation_color(height_m)
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
    status.text = tr("MAP_STATUS_READY") % [
        seed,
        MAP_WIDTH,
        MAP_HEIGHT,
        min_height,
        max_height
    ]
    generate_button.disabled = false

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

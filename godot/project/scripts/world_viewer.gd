extends Node3D

@onready var sim: WorldSimulationNode = $Simulation
@onready var camera: Camera3D = $Camera3D
@onready var status: Label = $HUD/StatusPanel/Margin/VBox/Status
@onready var help: Label = $HUD/StatusPanel/Margin/VBox/Help

const REFRESH_INTERVAL := 0.25
const ORBIT_SPEED := 1.35
const MOUSE_ORBIT_SENSITIVITY := 0.006
const KEYBOARD_ZOOM_SPEED := 2.2
const MIN_CAMERA_DISTANCE := 3.15
const MAX_CAMERA_DISTANCE := 12.0
const SPEED_STEPS := [1, 6, 24, 72]

var multimesh_instance: MultiMeshInstance3D
var multimesh: MultiMesh
var elapsed := 0.0
var yaw := 0.0
var pitch := 0.12
var camera_distance := 4.2
var mouse_orbiting := false
var paused := false
var speed_index := 1

func _ready() -> void:
    sim.initialize(42)
    _create_planet_visual()
    _create_multimesh()
    _update_camera()
    help.text = tr("HUD_CONTROLS")
    _advance_and_update(24)

func _process(delta: float) -> void:
    var orbit_x := Input.get_axis("camera_orbit_left", "camera_orbit_right")
    var orbit_y := Input.get_axis("camera_orbit_up", "camera_orbit_down")
    if !is_zero_approx(orbit_x) or !is_zero_approx(orbit_y):
        yaw -= orbit_x * ORBIT_SPEED * delta
        pitch = clampf(pitch - orbit_y * ORBIT_SPEED * delta, -1.25, 1.25)
        _update_camera()

    if Input.is_action_pressed("camera_zoom_in"):
        _zoom(-KEYBOARD_ZOOM_SPEED * delta)
    if Input.is_action_pressed("camera_zoom_out"):
        _zoom(KEYBOARD_ZOOM_SPEED * delta)

    elapsed += delta
    if elapsed >= REFRESH_INTERVAL:
        elapsed = fmod(elapsed, REFRESH_INTERVAL)
        sim.set_focus_direction(camera.global_position.normalized())
        if !paused:
            sim.step_hours(SPEED_STEPS[speed_index])
        _update_visual()

func _unhandled_input(event: InputEvent) -> void:
    var handled := false

    if event.is_action_pressed("simulation_pause"):
        paused = !paused
        _update_status()
        handled = true
    elif event.is_action_pressed("simulation_slower"):
        speed_index = maxi(0, speed_index - 1)
        _update_status()
        handled = true
    elif event.is_action_pressed("simulation_faster"):
        speed_index = mini(SPEED_STEPS.size() - 1, speed_index + 1)
        _update_status()
        handled = true
    elif event.is_action_pressed("camera_reset"):
        yaw = 0.0
        pitch = 0.12
        camera_distance = 4.2
        _update_camera()
        handled = true
    elif event is InputEventMouseButton:
        if event.button_index == MOUSE_BUTTON_RIGHT:
            mouse_orbiting = event.pressed
            handled = true
        elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
            _zoom(-0.55 * event.factor)
            handled = true
        elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
            _zoom(0.55 * event.factor)
            handled = true
    elif event is InputEventMouseMotion and mouse_orbiting:
        yaw -= event.relative.x * MOUSE_ORBIT_SENSITIVITY
        pitch = clampf(pitch - event.relative.y * MOUSE_ORBIT_SENSITIVITY, -1.25, 1.25)
        _update_camera()
        handled = true

    if handled:
        get_viewport().set_input_as_handled()

func _create_planet_visual() -> void:
    var planet := MeshInstance3D.new()
    planet.name = "PlanetBase"

    var mesh := SphereMesh.new()
    mesh.radius = 2.50
    mesh.height = 5.0
    mesh.radial_segments = 64
    mesh.rings = 32

    var material := StandardMaterial3D.new()
    material.albedo_color = Color(0.025, 0.055, 0.10)
    material.roughness = 0.86
    mesh.material = material

    planet.mesh = mesh
    add_child(planet)

func _create_multimesh() -> void:
    multimesh_instance = MultiMeshInstance3D.new()
    multimesh_instance.name = "SimulationCells"
    multimesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
    add_child(multimesh_instance)

    var mesh := SphereMesh.new()
    mesh.radius = 0.022
    mesh.height = 0.044
    mesh.radial_segments = 4
    mesh.rings = 2

    var material := StandardMaterial3D.new()
    material.albedo_color = Color.WHITE
    material.vertex_color_use_as_albedo = true
    material.roughness = 0.72
    mesh.material = material

    multimesh = MultiMesh.new()
    multimesh.transform_format = MultiMesh.TRANSFORM_3D
    multimesh.use_colors = true
    multimesh.mesh = mesh
    multimesh.custom_aabb = AABB(Vector3(-2.8, -2.8, -2.8), Vector3(5.6, 5.6, 5.6))
    multimesh_instance.multimesh = multimesh

func _advance_and_update(hours: int) -> void:
    sim.step_hours(hours)
    _update_visual()

func _update_visual() -> void:
    var packet := sim.get_render_packet()
    if packet.is_empty():
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        return

    var positions: PackedVector3Array = packet["positions"]
    var temps: PackedFloat32Array = packet["temperature_c"]
    var vegetation: PackedFloat32Array = packet["vegetation_kg_m2"]
    var mana: PackedFloat32Array = packet["mana_j_m2"]
    var levels: PackedInt32Array = packet["levels"]

    if multimesh.instance_count != positions.size():
        multimesh.instance_count = positions.size()

    for i in range(positions.size()):
        var level_scale := 0.7 + 0.08 * float(levels[i])
        multimesh.set_instance_transform(
            i,
            Transform3D(Basis().scaled(Vector3.ONE * level_scale), positions[i] * 2.6)
        )
        var t: float = clampf((float(temps[i]) + 25.0) / 60.0, 0.0, 1.0)
        var v: float = clampf(float(vegetation[i]) / 5.0, 0.0, 1.0)
        var m: float = clampf(float(mana[i]) / 4000000.0, 0.0, 1.0)
        multimesh.set_instance_color(
            i,
            Color(0.15 + 0.75 * t, 0.12 + 0.78 * v, 0.20 + 0.65 * m, 1.0)
        )

    multimesh.visible_instance_count = positions.size()
    _update_status()

func _update_status() -> void:
    if paused:
        status.text = tr("HUD_STATUS_PAUSED") % [
            sim.get_tick(),
            multimesh.visible_instance_count,
            sim.get_field_descriptors().size()
        ]
    else:
        status.text = tr("HUD_STATUS_RUNNING") % [
            sim.get_tick(),
            multimesh.visible_instance_count,
            sim.get_field_descriptors().size(),
            SPEED_STEPS[speed_index]
        ]

func _update_camera() -> void:
    var cp := cos(pitch)
    camera.position = Vector3(
        sin(yaw) * cp,
        sin(pitch),
        cos(yaw) * cp
    ) * camera_distance
    camera.look_at(Vector3.ZERO, Vector3.UP)

func _zoom(delta_distance: float) -> void:
    var next_distance := clampf(
        camera_distance + delta_distance,
        MIN_CAMERA_DISTANCE,
        MAX_CAMERA_DISTANCE
    )
    if !is_equal_approx(next_distance, camera_distance):
        camera_distance = next_distance
        _update_camera()

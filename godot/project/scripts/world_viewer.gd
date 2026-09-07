extends Node3D

@onready var sim: WorldSimulationNode = $Simulation
@onready var camera: Camera3D = $Camera3D
@onready var status: Label = $HUD/Status

var multimesh_instance: MultiMeshInstance3D
var elapsed := 0.0

func _ready() -> void:
    sim.initialize(42)
    multimesh_instance = MultiMeshInstance3D.new()
    add_child(multimesh_instance)
    _advance_and_rebuild(24)

func _process(delta: float) -> void:
    elapsed += delta
    rotate_y(delta * 0.08)
    if elapsed >= 0.25:
        elapsed = 0.0
        # The visible hemisphere center is the direction from the planet origin to the camera.
        sim.set_focus_direction(camera.global_position.normalized())
        _advance_and_rebuild(6)

func _advance_and_rebuild(hours: int) -> void:
    sim.step_hours(hours)
    var packet := sim.get_render_packet()
    if packet.is_empty():
        status.text = "WorldSim error: %s" % sim.get_last_error()
        return

    var positions: PackedVector3Array = packet["positions"]
    var temps: PackedFloat32Array = packet["temperature_c"]
    var vegetation: PackedFloat32Array = packet["vegetation_kg_m2"]
    var mana: PackedFloat32Array = packet["mana_j_m2"]
    var levels: PackedInt32Array = packet["levels"]

    var mesh := SphereMesh.new()
    mesh.radius = 0.022
    mesh.height = 0.044
    mesh.radial_segments = 4
    mesh.rings = 2

    var material := StandardMaterial3D.new()
    material.albedo_color = Color.WHITE
    material.vertex_color_use_as_albedo = true
    mesh.material = material

    var mm := MultiMesh.new()
    mm.transform_format = MultiMesh.TRANSFORM_3D
    mm.use_colors = true
    mm.mesh = mesh
    mm.instance_count = positions.size()

    for i in range(positions.size()):
        var level_scale := 0.7 + 0.08 * float(levels[i])
        mm.set_instance_transform(i, Transform3D(Basis().scaled(Vector3.ONE * level_scale), positions[i] * 2.6))
        var t: float = clampf((float(temps[i]) + 25.0) / 60.0, 0.0, 1.0)
        var v: float = clampf(float(vegetation[i]) / 5.0, 0.0, 1.0)
        var m: float = clampf(float(mana[i]) / 4000000.0, 0.0, 1.0)
        mm.set_instance_color(i, Color(0.15 + 0.75 * t, 0.12 + 0.78 * v, 0.20 + 0.65 * m, 1.0))
    multimesh_instance.multimesh = mm

    status.text = "tick: %d    active cells: %d    fields: %d" % [
        sim.get_tick(), positions.size(), sim.get_field_descriptors().size()
    ]

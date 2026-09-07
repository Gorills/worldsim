extends Node3D

@onready var sim: WorldSimulationNode = $Simulation
@onready var terrain_root: Node3D = $Terrain
@onready var player: CharacterBody3D = $Player
@onready var head: Node3D = $Player/Head
@onready var status: Label = $HUD/StatusPanel/Margin/VBox/Status
@onready var help: Label = $HUD/StatusPanel/Margin/VBox/Help

const CHUNK_SIZE_M := 256.0
const CHUNK_RESOLUTION := 33
const SAMPLE_SPACING_M := CHUNK_SIZE_M / float(CHUNK_RESOLUTION - 1)
const VISIBLE_RADIUS := 3
const KEEP_RADIUS := 4
const MAX_CHUNKS_PER_FRAME := 1
const ORIGIN_SHIFT_THRESHOLD_M := 1024.0

const WALK_SPEED_M_S := 8.0
const JUMP_SPEED_M_S := 7.0
const GRAVITY_M_S2 := 22.0
const MOUSE_SENSITIVITY := 0.0022
const SIM_FOCUS_INTERVAL := 0.25

var terrain_material: StandardMaterial3D
var chunks: Dictionary = {}
var pending_chunks: Array[Vector2i] = []
var current_chunk := Vector2i(0, 0)

# Logical projected coordinates stay in 64-bit GDScript floats. Scene-tree coordinates
# stay near zero so the stock single-precision Godot build remains stable at world scale.
var origin_east_m := 0.0
var origin_north_m := 0.0
var origin_height_m := 0.0
var sim_focus_elapsed := 0.0
var status_elapsed := 0.0
var pitch := 0.0

func _ready() -> void:
    sim.initialize_terrain_world(42)
    if !sim.get_last_error().is_empty():
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        return

    terrain_material = StandardMaterial3D.new()
    terrain_material.vertex_color_use_as_albedo = true
    terrain_material.roughness = 0.95

    origin_height_m = sim.sample_terrain_height(0.0, 0.0)
    _create_chunk(Vector2i.ZERO)
    player.position = Vector3(0.0, 1.25, 0.0)
    _queue_visible_chunks(Vector2i.ZERO)
    help.text = tr("HUD_CONTROLS")
    _update_status()

    if DisplayServer.get_name() != "headless":
        Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

func _process(delta: float) -> void:
    for _i in range(MAX_CHUNKS_PER_FRAME):
        if pending_chunks.is_empty():
            break
        _create_chunk(pending_chunks.pop_front())

    sim_focus_elapsed += delta
    if sim_focus_elapsed >= SIM_FOCUS_INTERVAL:
        sim_focus_elapsed = fmod(sim_focus_elapsed, SIM_FOCUS_INTERVAL)
        var east_m := origin_east_m + float(player.position.x)
        var north_m := origin_north_m + float(player.position.z)
        sim.set_focus_projected(east_m, north_m)
        sim.step_hours(1)

    status_elapsed += delta
    if status_elapsed >= 0.25:
        status_elapsed = 0.0
        _update_status()

func _physics_process(delta: float) -> void:
    var input_2d := Input.get_vector("move_left", "move_right", "move_forward", "move_back")
    var basis := player.global_transform.basis
    var forward := -basis.z
    var right := basis.x
    forward.y = 0.0
    right.y = 0.0
    forward = forward.normalized()
    right = right.normalized()
    var motion := (right * input_2d.x - forward * input_2d.y)
    if motion.length_squared() > 1.0:
        motion = motion.normalized()

    player.velocity.x = motion.x * WALK_SPEED_M_S
    player.velocity.z = motion.z * WALK_SPEED_M_S
    if player.is_on_floor():
        if Input.is_action_just_pressed("jump"):
            player.velocity.y = JUMP_SPEED_M_S
    else:
        player.velocity.y -= GRAVITY_M_S2 * delta
    player.move_and_slide()

    _maybe_shift_origin()

    var next_chunk := _world_chunk(
        origin_east_m + float(player.position.x),
        origin_north_m + float(player.position.z)
    )
    if next_chunk != current_chunk:
        current_chunk = next_chunk
        _queue_visible_chunks(current_chunk)
        _trim_chunks(current_chunk)

func _unhandled_input(event: InputEvent) -> void:
    if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
        Input.mouse_mode = (
            Input.MOUSE_MODE_VISIBLE
            if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
            else Input.MOUSE_MODE_CAPTURED
        )
        get_viewport().set_input_as_handled()
        return

    if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
        Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
        get_viewport().set_input_as_handled()
        return

    if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
        player.rotate_y(-event.relative.x * MOUSE_SENSITIVITY)
        pitch = clampf(pitch - event.relative.y * MOUSE_SENSITIVITY, -1.45, 1.45)
        head.rotation.x = pitch
        get_viewport().set_input_as_handled()

func _world_chunk(east_m: float, north_m: float) -> Vector2i:
    return Vector2i(
        roundi(east_m / CHUNK_SIZE_M),
        roundi(north_m / CHUNK_SIZE_M)
    )

func _queue_visible_chunks(center: Vector2i) -> void:
    pending_chunks.clear()
    for ring in range(VISIBLE_RADIUS + 1):
        for dz in range(-ring, ring + 1):
            for dx in range(-ring, ring + 1):
                if maxi(absi(dx), absi(dz)) != ring:
                    continue
                var coord := center + Vector2i(dx, dz)
                if !chunks.has(coord):
                    pending_chunks.push_back(coord)

func _trim_chunks(center: Vector2i) -> void:
    for key in chunks.keys():
        var coord: Vector2i = key
        var delta := coord - center
        if maxi(absi(delta.x), absi(delta.y)) > KEEP_RADIUS:
            var chunk: Node3D = chunks[coord]
            chunk.queue_free()
            chunks.erase(coord)

func _create_chunk(coord: Vector2i) -> void:
    if chunks.has(coord):
        return

    var center_east_m := float(coord.x) * CHUNK_SIZE_M
    var center_north_m := float(coord.y) * CHUNK_SIZE_M
    var heights := sim.sample_terrain_patch(
        center_east_m,
        center_north_m,
        SAMPLE_SPACING_M,
        CHUNK_RESOLUTION
    )
    if heights.size() != CHUNK_RESOLUTION * CHUNK_RESOLUTION:
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        return

    var chunk := Node3D.new()
    chunk.name = "Chunk_%d_%d" % [coord.x, coord.y]
    chunk.position = _chunk_local_position(coord)
    terrain_root.add_child(chunk)

    var mesh_instance := MeshInstance3D.new()
    mesh_instance.mesh = _build_chunk_mesh(heights)
    mesh_instance.material_override = terrain_material
    mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
    chunk.add_child(mesh_instance)

    var body := StaticBody3D.new()
    var collision := CollisionShape3D.new()
    var shape := HeightMapShape3D.new()
    shape.map_width = CHUNK_RESOLUTION
    shape.map_depth = CHUNK_RESOLUTION
    var collision_heights := PackedFloat32Array()
    collision_heights.resize(heights.size())
    for i in range(heights.size()):
        collision_heights[i] = heights[i] / SAMPLE_SPACING_M
    shape.map_data = collision_heights
    collision.shape = shape
    collision.scale = Vector3.ONE * SAMPLE_SPACING_M
    body.add_child(collision)
    chunk.add_child(body)

    chunks[coord] = chunk

func _build_chunk_mesh(heights: PackedFloat32Array) -> ArrayMesh:
    var vertex_count := CHUNK_RESOLUTION * CHUNK_RESOLUTION
    var vertices := PackedVector3Array()
    var normals := PackedVector3Array()
    var colors := PackedColorArray()
    vertices.resize(vertex_count)
    normals.resize(vertex_count)
    colors.resize(vertex_count)

    var half := CHUNK_SIZE_M * 0.5
    for z in range(CHUNK_RESOLUTION):
        for x in range(CHUNK_RESOLUTION):
            var i := z * CHUNK_RESOLUTION + x
            var height := float(heights[i])
            vertices[i] = Vector3(
                float(x) * SAMPLE_SPACING_M - half,
                height,
                float(z) * SAMPLE_SPACING_M - half
            )

            var left := float(heights[z * CHUNK_RESOLUTION + maxi(x - 1, 0)])
            var right_h := float(heights[z * CHUNK_RESOLUTION + mini(x + 1, CHUNK_RESOLUTION - 1)])
            var down := float(heights[maxi(z - 1, 0) * CHUNK_RESOLUTION + x])
            var up := float(heights[mini(z + 1, CHUNK_RESOLUTION - 1) * CHUNK_RESOLUTION + x])
            normals[i] = Vector3(
                left - right_h,
                2.0 * SAMPLE_SPACING_M,
                down - up
            ).normalized()

            if height < 0.0:
                var depth_t := clampf(-height / 5000.0, 0.0, 1.0)
                colors[i] = Color(0.25, 0.27, 0.28).lerp(Color(0.12, 0.14, 0.16), depth_t)
            else:
                var elevation_t := clampf(height / 4500.0, 0.0, 1.0)
                colors[i] = Color(0.34, 0.31, 0.27).lerp(Color(0.62, 0.60, 0.56), elevation_t)

    var indices := PackedInt32Array()
    indices.resize((CHUNK_RESOLUTION - 1) * (CHUNK_RESOLUTION - 1) * 6)
    var index_cursor := 0
    for z in range(CHUNK_RESOLUTION - 1):
        for x in range(CHUNK_RESOLUTION - 1):
            var i0 := z * CHUNK_RESOLUTION + x
            var i1 := i0 + 1
            var i2 := i0 + CHUNK_RESOLUTION
            var i3 := i2 + 1
            indices[index_cursor] = i0
            indices[index_cursor + 1] = i2
            indices[index_cursor + 2] = i1
            indices[index_cursor + 3] = i1
            indices[index_cursor + 4] = i2
            indices[index_cursor + 5] = i3
            index_cursor += 6

    var arrays := []
    arrays.resize(Mesh.ARRAY_MAX)
    arrays[Mesh.ARRAY_VERTEX] = vertices
    arrays[Mesh.ARRAY_NORMAL] = normals
    arrays[Mesh.ARRAY_COLOR] = colors
    arrays[Mesh.ARRAY_INDEX] = indices

    var mesh := ArrayMesh.new()
    mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
    return mesh

func _chunk_local_position(coord: Vector2i) -> Vector3:
    return Vector3(
        float(coord.x) * CHUNK_SIZE_M - origin_east_m,
        -origin_height_m,
        float(coord.y) * CHUNK_SIZE_M - origin_north_m
    )

func _maybe_shift_origin() -> void:
    if absf(player.position.x) < ORIGIN_SHIFT_THRESHOLD_M and absf(player.position.z) < ORIGIN_SHIFT_THRESHOLD_M:
        return

    var old_origin_height := origin_height_m
    origin_east_m += float(player.position.x)
    origin_north_m += float(player.position.z)
    origin_height_m = sim.sample_terrain_height(origin_east_m, origin_north_m)

    player.position.x = 0.0
    player.position.z = 0.0
    player.position.y += old_origin_height - origin_height_m

    for key in chunks.keys():
        var coord: Vector2i = key
        var chunk: Node3D = chunks[coord]
        chunk.position = _chunk_local_position(coord)

func _update_status() -> void:
    var east_m := origin_east_m + float(player.position.x)
    var north_m := origin_north_m + float(player.position.z)
    status.text = tr("HUD_STATUS_RUNNING") % [
        east_m / 1000.0,
        north_m / 1000.0,
        chunks.size(),
        int(Engine.get_frames_per_second()),
        sim.get_tick()
    ]

extends Node3D

@onready var sim: WorldSimulationNode = $Simulation
@onready var terrain_root: Node3D = $Terrain
@onready var player: CharacterBody3D = $Player
@onready var head: Node3D = $Player/Head
@onready var status: Label = $HUD/StatusPanel/Margin/VBox/Status
@onready var help: Label = $HUD/StatusPanel/Margin/VBox/Help
@onready var minimap_view: TextureRect = $HUD/MiniMapPanel/Margin/VBox/MapFrame/Inset/Layers/Map
@onready var minimap_overlay: Control = $HUD/MiniMapPanel/Margin/VBox/MapFrame/Inset/Layers/Overlay

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
const SURVEY_SPEEDS_M_S := [250.0, 2500.0, 25000.0, 250000.0]
const SURVEY_BOOST_MULTIPLIER := 4.0
const SURVEY_MIN_CLEARANCE_M := 250.0
const PLAYER_GROUND_CLEARANCE_M := 1.25
const MINIMAP_WIDTH := 320
const MINIMAP_HEIGHT := 160
const MINIMAP_HEADING_SAMPLE_M := 50000.0

var terrain_material: StandardMaterial3D
var chunks: Dictionary = {}
var dirty_chunks: Dictionary = {}
var pending_chunks: Array[Vector2i] = []
var current_chunk := Vector2i(0, 0)
var terrain_revision := 0

# Logical projected coordinates stay in 64-bit GDScript floats. Scene-tree coordinates
# stay near zero so the stock single-precision Godot build remains stable at world scale.
var origin_east_m := 0.0
var origin_north_m := 0.0
var origin_height_m := 0.0
var sim_focus_elapsed := 0.0
var status_elapsed := 0.0
var pitch := 0.0
var survey_flight_enabled := false
var survey_speed_index := 1
var walking_collision_mask := 1

func _ready() -> void:
    sim.initialize_terrain_world(42)
    if !sim.get_last_error().is_empty():
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        return

    terrain_material = StandardMaterial3D.new()
    terrain_material.vertex_color_use_as_albedo = true
    terrain_material.roughness = 0.95

    terrain_revision = sim.get_terrain_revision()
    origin_height_m = sim.sample_terrain_height(0.0, 0.0)
    _create_chunk(Vector2i.ZERO)
    player.position = Vector3(0.0, PLAYER_GROUND_CLEARANCE_M, 0.0)
    walking_collision_mask = player.collision_mask
    pitch = -0.22
    head.rotation.x = pitch
    if !_create_world_minimap():
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        return
    _update_minimap_marker()
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
        var old_origin_height := origin_height_m
        var old_ground_height := sim.sample_terrain_height(east_m, north_m)
        var was_grounded := !survey_flight_enabled and player.is_on_floor()
        sim.set_focus_projected(east_m, north_m)
        sim.step_hours(1)
        _refresh_terrain_revision(old_origin_height, old_ground_height, was_grounded)

    status_elapsed += delta
    if status_elapsed >= 0.25:
        status_elapsed = 0.0
        _update_status()

    _update_minimap_marker()

func _physics_process(delta: float) -> void:
    var input_2d := Input.get_vector("move_left", "move_right", "move_forward", "move_back")
    if survey_flight_enabled:
        _move_survey_flight(input_2d, delta)
    else:
        _move_walker(input_2d, delta)

    _maybe_shift_origin()
    _refresh_streaming_center()

func _move_walker(input_2d: Vector2, delta: float) -> void:
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

func _move_survey_flight(input_2d: Vector2, delta: float) -> void:
    var basis := head.global_transform.basis
    var forward := -basis.z
    var right := basis.x
    var vertical := (
        Input.get_action_strength("jump")
        - Input.get_action_strength("survey_descend")
    )
    var motion := right * input_2d.x - forward * input_2d.y + Vector3.UP * vertical
    if motion.length_squared() > 1.0:
        motion = motion.normalized()

    var speed := _survey_speed_m_s()
    if Input.is_action_pressed("survey_boost"):
        speed *= SURVEY_BOOST_MULTIPLIER
    var travel := motion * speed * delta
    player.velocity = Vector3.ZERO
    if travel.is_zero_approx():
        return

    # Integrate horizontal flight directly into the 64-bit logical origin. Even
    # the fastest tier therefore never puts a multi-million-meter coordinate in
    # the stock single-precision scene tree for one frame.
    var old_origin_height := origin_height_m
    origin_east_m += travel.x
    origin_north_m += travel.z
    origin_height_m = sim.sample_terrain_height(origin_east_m, origin_north_m)
    player.position.y += travel.y + old_origin_height - origin_height_m
    for key in chunks.keys():
        var coord: Vector2i = key
        var chunk: Node3D = chunks[coord]
        chunk.position = _chunk_local_position(coord)

func _refresh_streaming_center() -> void:
    var next_chunk := _world_chunk(
        origin_east_m + float(player.position.x),
        origin_north_m + float(player.position.z)
    )
    if next_chunk != current_chunk:
        current_chunk = next_chunk
        # Survey flight can cross many chunks in one physics frame. Build the
        # destination tile immediately; the remaining neighborhood stays under
        # the normal one-chunk-per-rendered-frame streaming budget.
        if survey_flight_enabled:
            _create_chunk(current_chunk)
        _queue_visible_chunks(current_chunk)
        _trim_chunks(current_chunk)

func _unhandled_input(event: InputEvent) -> void:
    if event.is_action_pressed("toggle_survey_flight"):
        _set_survey_flight_enabled(!survey_flight_enabled)
        get_viewport().set_input_as_handled()
        return

    if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
        Input.mouse_mode = (
            Input.MOUSE_MODE_VISIBLE
            if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED
            else Input.MOUSE_MODE_CAPTURED
        )
        get_viewport().set_input_as_handled()
        return

    if event is InputEventMouseButton and event.pressed:
        if survey_flight_enabled and event.button_index == MOUSE_BUTTON_WHEEL_UP:
            _cycle_survey_speed(1)
            get_viewport().set_input_as_handled()
            return
        if survey_flight_enabled and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
            _cycle_survey_speed(-1)
            get_viewport().set_input_as_handled()
            return
        if event.button_index == MOUSE_BUTTON_LEFT:
            Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
            get_viewport().set_input_as_handled()
            return

    if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
        player.rotate_y(-event.relative.x * MOUSE_SENSITIVITY)
        pitch = clampf(pitch - event.relative.y * MOUSE_SENSITIVITY, -1.45, 1.45)
        head.rotation.x = pitch
        get_viewport().set_input_as_handled()

func _set_survey_flight_enabled(enabled: bool) -> void:
    if survey_flight_enabled == enabled:
        return

    survey_flight_enabled = enabled
    player.velocity = Vector3.ZERO
    if enabled:
        player.motion_mode = CharacterBody3D.MOTION_MODE_FLOATING
        player.collision_mask = 0
        var ground_local_y := _ground_local_y()
        player.position.y = maxf(player.position.y, ground_local_y + SURVEY_MIN_CLEARANCE_M)
    else:
        # The destination collision must exist before grounded movement resumes.
        current_chunk = _world_chunk(
            origin_east_m + float(player.position.x),
            origin_north_m + float(player.position.z)
        )
        _create_chunk(current_chunk)
        player.position.y = _ground_local_y() + PLAYER_GROUND_CLEARANCE_M
        player.collision_mask = walking_collision_mask
        player.motion_mode = CharacterBody3D.MOTION_MODE_GROUNDED

    _update_status()

func _cycle_survey_speed(direction: int) -> void:
    survey_speed_index = clampi(
        survey_speed_index + direction,
        0,
        SURVEY_SPEEDS_M_S.size() - 1
    )
    _update_status()

func _survey_speed_m_s() -> float:
    return float(SURVEY_SPEEDS_M_S[survey_speed_index])

func _ground_local_y() -> float:
    var east_m := origin_east_m + float(player.position.x)
    var north_m := origin_north_m + float(player.position.z)
    return sim.sample_terrain_height(east_m, north_m) - origin_height_m

func _create_world_minimap() -> bool:
    var heights := sim.sample_terrain_equirectangular(MINIMAP_WIDTH, MINIMAP_HEIGHT)
    if heights.size() != MINIMAP_WIDTH * MINIMAP_HEIGHT:
        return false

    var pixels := PackedByteArray()
    pixels.resize(MINIMAP_WIDTH * MINIMAP_HEIGHT * 3)
    for y in range(MINIMAP_HEIGHT):
        for x in range(MINIMAP_WIDTH):
            var index := y * MINIMAP_WIDTH + x
            var height_m := float(heights[index])
            var left := float(heights[y * MINIMAP_WIDTH + (x - 1 + MINIMAP_WIDTH) % MINIMAP_WIDTH])
            var right_h := float(heights[y * MINIMAP_WIDTH + (x + 1) % MINIMAP_WIDTH])
            var up := float(heights[maxi(y - 1, 0) * MINIMAP_WIDTH + x])
            var down := float(heights[mini(y + 1, MINIMAP_HEIGHT - 1) * MINIMAP_WIDTH + x])
            var color := _minimap_elevation_color(height_m)

            var coast := height_m >= 0.0 and (
                left < 0.0 or right_h < 0.0 or up < 0.0 or down < 0.0
            )
            if coast:
                color = Color(0.80, 0.76, 0.50).lerp(color, 0.32)
            else:
                var shade := clampf(
                    (left - right_h) * 0.000045 + (down - up) * 0.000035,
                    -0.16,
                    0.16
                )
                color = color.lightened(shade) if shade >= 0.0 else color.darkened(-shade)

            var byte_index := index * 3
            pixels[byte_index] = clampi(roundi(color.r * 255.0), 0, 255)
            pixels[byte_index + 1] = clampi(roundi(color.g * 255.0), 0, 255)
            pixels[byte_index + 2] = clampi(roundi(color.b * 255.0), 0, 255)

    var image := Image.create_from_data(
        MINIMAP_WIDTH,
        MINIMAP_HEIGHT,
        false,
        Image.FORMAT_RGB8,
        pixels
    )
    minimap_view.texture = ImageTexture.create_from_image(image)
    return true

func _minimap_elevation_color(height_m: float) -> Color:
    if height_m < 0.0:
        var depth := clampf(-height_m / 6000.0, 0.0, 1.0)
        return Color(0.075, 0.30, 0.48).lerp(Color(0.012, 0.035, 0.09), depth)
    if height_m < 900.0:
        var lowland := clampf(height_m / 900.0, 0.0, 1.0)
        return Color(0.20, 0.46, 0.25).lerp(Color(0.48, 0.50, 0.25), lowland)
    if height_m < 3000.0:
        var highland := clampf((height_m - 900.0) / 2100.0, 0.0, 1.0)
        return Color(0.48, 0.50, 0.25).lerp(Color(0.43, 0.33, 0.26), highland)
    var mountain := clampf((height_m - 3000.0) / 3500.0, 0.0, 1.0)
    return Color(0.43, 0.33, 0.26).lerp(Color(0.92, 0.94, 0.94), mountain)

func _update_minimap_marker() -> void:
    var east_m := origin_east_m + float(player.position.x)
    var north_m := origin_north_m + float(player.position.z)
    var forward := -player.global_transform.basis.z
    forward.y = 0.0
    forward = forward.normalized()

    var current_direction := sim.projected_to_direction(east_m, north_m)
    var ahead_direction := sim.projected_to_direction(
        east_m + forward.x * MINIMAP_HEADING_SAMPLE_M,
        north_m + forward.z * MINIMAP_HEADING_SAMPLE_M
    )
    var marker_uv := _sphere_direction_to_map_uv(current_direction)
    var ahead_uv := _sphere_direction_to_map_uv(ahead_direction)
    var heading_uv := ahead_uv - marker_uv
    if heading_uv.x > 0.5:
        heading_uv.x -= 1.0
    elif heading_uv.x < -0.5:
        heading_uv.x += 1.0
    minimap_overlay.call("set_marker", marker_uv, heading_uv)

func _sphere_direction_to_map_uv(direction: Vector3) -> Vector2:
    var longitude := atan2(direction.y, direction.x)
    var latitude := asin(clampf(direction.z, -1.0, 1.0))
    return Vector2(longitude / TAU + 0.5, 0.5 - latitude / PI)

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
                if !chunks.has(coord) or dirty_chunks.has(coord):
                    pending_chunks.push_back(coord)

func _trim_chunks(center: Vector2i) -> void:
    for key in chunks.keys():
        var coord: Vector2i = key
        var delta := coord - center
        if maxi(absi(delta.x), absi(delta.y)) > KEEP_RADIUS:
            var chunk: Node3D = chunks[coord]
            chunk.queue_free()
            chunks.erase(coord)
            dirty_chunks.erase(coord)

func _create_chunk(coord: Vector2i) -> void:
    if chunks.has(coord) and !dirty_chunks.has(coord):
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

    var shape := HeightMapShape3D.new()
    shape.map_width = CHUNK_RESOLUTION
    shape.map_depth = CHUNK_RESOLUTION
    var collision_heights := PackedFloat32Array()
    collision_heights.resize(heights.size())
    for i in range(heights.size()):
        collision_heights[i] = heights[i] / SAMPLE_SPACING_M
    shape.map_data = collision_heights

    if chunks.has(coord):
        var existing_chunk: Node3D = chunks[coord]
        var existing_mesh := existing_chunk.get_node("Mesh") as MeshInstance3D
        var existing_collision := existing_chunk.get_node("Body/Collision") as CollisionShape3D
        existing_mesh.mesh = _build_chunk_mesh(heights)
        existing_collision.shape = shape
        existing_chunk.position = _chunk_local_position(coord)
    else:
        var chunk := Node3D.new()
        chunk.name = "Chunk_%d_%d" % [coord.x, coord.y]
        chunk.position = _chunk_local_position(coord)
        terrain_root.add_child(chunk)

        var mesh_instance := MeshInstance3D.new()
        mesh_instance.name = "Mesh"
        mesh_instance.mesh = _build_chunk_mesh(heights)
        mesh_instance.material_override = terrain_material
        mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        chunk.add_child(mesh_instance)

        var body := StaticBody3D.new()
        body.name = "Body"
        var collision := CollisionShape3D.new()
        collision.name = "Collision"
        collision.shape = shape
        collision.scale = Vector3.ONE * SAMPLE_SPACING_M
        body.add_child(collision)
        chunk.add_child(body)

        chunks[coord] = chunk

    dirty_chunks.erase(coord)

func _refresh_terrain_revision(
    old_origin_height: float,
    old_ground_height: float,
    was_grounded: bool
) -> void:
    var next_revision := sim.get_terrain_revision()
    if next_revision == terrain_revision:
        return

    terrain_revision = next_revision
    var east_m := origin_east_m + float(player.position.x)
    var north_m := origin_north_m + float(player.position.z)
    origin_height_m = sim.sample_terrain_height(origin_east_m, origin_north_m)
    player.position.y += old_origin_height - origin_height_m
    if was_grounded:
        var new_ground_height := sim.sample_terrain_height(east_m, north_m)
        player.position.y += new_ground_height - old_ground_height

    for key in chunks.keys():
        var coord: Vector2i = key
        var chunk: Node3D = chunks[coord]
        chunk.position = _chunk_local_position(coord)
        dirty_chunks[coord] = true

    # Keep the collision directly under the player synchronized immediately;
    # the remaining visible chunks retain the one-update-per-frame budget.
    _create_chunk(current_chunk)
    _queue_visible_chunks(current_chunk)

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

    # Godot 4.7 culls counter-clockwise triangles. Clockwise from +Y is
    # (x, z) -> (x+1, z) -> (x, z+1), matching the official ArrayMesh example.
    # https://docs.godotengine.org/en/4.7/classes/class_arraymesh.html
    # https://docs.godotengine.org/en/4.7/tutorials/3d/procedural_geometry/arraymesh.html
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
            indices[index_cursor + 1] = i1
            indices[index_cursor + 2] = i2
            indices[index_cursor + 3] = i1
            indices[index_cursor + 4] = i3
            indices[index_cursor + 5] = i2
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
    var mode_text := tr("HUD_MODE_SURVEY") if survey_flight_enabled else tr("HUD_MODE_WALK")
    var speed_m_s := _survey_speed_m_s() if survey_flight_enabled else WALK_SPEED_M_S
    if survey_flight_enabled and Input.is_action_pressed("survey_boost"):
        speed_m_s *= SURVEY_BOOST_MULTIPLIER
    var speed_text := (
        "%.1f km/s" % (speed_m_s / 1000.0)
        if speed_m_s >= 1000.0
        else "%.0f m/s" % speed_m_s
    )
    status.text = tr("HUD_STATUS_RUNNING") % [
        east_m / 1000.0,
        north_m / 1000.0,
        mode_text,
        speed_text,
        chunks.size(),
        int(Engine.get_frames_per_second()),
        sim.get_tick()
    ]

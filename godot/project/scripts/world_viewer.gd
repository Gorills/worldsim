extends Node3D

const SurfaceVisual = preload("res://scripts/surface_visual.gd")

@onready var sim: WorldSimulationNode = $Simulation
@onready var terrain_root: Node3D = $Terrain
@onready var distant_terrain: Node3D = $DistantTerrain
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
const SURFACE_REFRESH_INTERVAL_S := 1.0
const TREE_CANDIDATES_PER_CHUNK := 64
const SHRUB_CANDIDATES_PER_CHUNK := 48
const TREE_VISUAL_HEIGHT_M := 7.0
const SHRUB_VISUAL_HEIGHT_M := 1.4

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
const MOUNTAIN_DEMO_CENTER_EAST_M := 5_573_000.0
const MOUNTAIN_DEMO_CENTER_NORTH_M := -1_800_300.0
const MOUNTAIN_DEMO_SAMPLE_SPACING_M := 625.0
const MOUNTAIN_DEMO_RESOLUTION := 65
const MOUNTAIN_VIEW_MIN_DISTANCE_M := 6_000.0
const MOUNTAIN_VIEW_MAX_DISTANCE_M := 10_000.0

var terrain_material: StandardMaterial3D
var tree_mesh: Mesh
var shrub_mesh: Mesh
var chunks: Dictionary = {}
var dirty_chunks: Dictionary = {}
var pending_chunks: Array[Vector2i] = []
var current_chunk := Vector2i(0, 0)
var terrain_revision := 0
var surface_revision := 0
var pending_surface_revision := 0
var surface_refresh_elapsed := 0.0

# Logical projected coordinates stay in 64-bit GDScript floats. Scene-tree coordinates
# stay near zero so the stock single-precision Godot build remains stable at world scale.
var origin_east_m := 0.0
var origin_north_m := 0.0
var origin_height_m := 0.0
var mountain_target_east_m := MOUNTAIN_DEMO_CENTER_EAST_M
var mountain_target_north_m := MOUNTAIN_DEMO_CENTER_NORTH_M
var sim_focus_elapsed := 0.0
var status_elapsed := 0.0
var pitch := 0.0
var survey_flight_enabled := false
var survey_speed_index := 1
var walking_collision_mask := 1

func _ready() -> void:
    sim.initialize(42)
    if !sim.get_last_error().is_empty():
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        return
    if !_select_mountain_demo_spawn():
        status.text = tr("HUD_STATUS_ERROR") % "mountain spawn terrain sampling failed"
        return
    current_chunk = _world_chunk(origin_east_m, origin_north_m)

    terrain_material = StandardMaterial3D.new()
    terrain_material.vertex_color_use_as_albedo = true
    terrain_material.roughness = 0.95
    _initialize_vegetation_meshes()

    terrain_revision = sim.get_terrain_revision()
    surface_revision = sim.get_surface_revision()
    pending_surface_revision = surface_revision
    origin_height_m = sim.sample_terrain_height(origin_east_m, origin_north_m)
    distant_terrain.call(
        "initialize",
        sim,
        origin_east_m,
        origin_north_m,
        origin_height_m
    )
    _create_chunk(current_chunk)
    player.position = Vector3(0.0, PLAYER_GROUND_CLEARANCE_M, 0.0)
    walking_collision_mask = player.collision_mask
    var target_east_delta := mountain_target_east_m - origin_east_m
    var target_north_delta := mountain_target_north_m - origin_north_m
    var target_distance_m := Vector2(
        target_east_delta,
        target_north_delta
    ).length()
    var target_height_m := sim.sample_terrain_height(
        mountain_target_east_m,
        mountain_target_north_m
    )
    player.rotation.y = atan2(-target_east_delta, target_north_delta)
    pitch = clampf(
        atan2(target_height_m - origin_height_m, maxf(target_distance_m, 1.0)),
        -0.35,
        0.35
    )
    head.rotation.x = pitch
    if !_create_world_minimap():
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        return
    _update_minimap_marker()
    _queue_visible_chunks(current_chunk)
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
        var north_m := origin_north_m - float(player.position.z)
        var old_origin_height := origin_height_m
        var old_ground_height := sim.sample_terrain_height(east_m, north_m)
        var was_grounded := !survey_flight_enabled and player.is_on_floor()
        sim.set_focus_projected(east_m, north_m)
        sim.step_hours(1)
        _refresh_terrain_revision(old_origin_height, old_ground_height, was_grounded)
        _observe_surface_revision()

    _refresh_surface_revision(delta)

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
    distant_terrain.call(
        "set_view_state",
        origin_east_m + float(player.position.x),
        origin_north_m - float(player.position.z),
        origin_east_m,
        origin_north_m,
        origin_height_m,
        survey_flight_enabled
    )

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
    origin_north_m -= travel.z
    origin_height_m = sim.sample_terrain_height(origin_east_m, origin_north_m)
    player.position.y += travel.y + old_origin_height - origin_height_m
    for key in chunks.keys():
        var coord: Vector2i = key
        var chunk: Node3D = chunks[coord]
        chunk.position = _chunk_local_position(coord)

func _refresh_streaming_center() -> void:
    var next_chunk := _world_chunk(
        origin_east_m + float(player.position.x),
        origin_north_m - float(player.position.z)
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
            origin_north_m - float(player.position.z)
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
    var north_m := origin_north_m - float(player.position.z)
    return sim.sample_terrain_height(east_m, north_m) - origin_height_m

func _create_world_minimap() -> bool:
    var heights := sim.sample_preview_terrain_equirectangular(
        MINIMAP_WIDTH,
        MINIMAP_HEIGHT
    )
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
    var north_m := origin_north_m - float(player.position.z)
    var forward := -player.global_transform.basis.z
    forward.y = 0.0
    forward = forward.normalized()

    var current_direction := sim.projected_to_direction(east_m, north_m)
    var ahead_direction := sim.projected_to_direction(
        east_m + forward.x * MINIMAP_HEADING_SAMPLE_M,
        north_m - forward.z * MINIMAP_HEADING_SAMPLE_M
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

func _select_mountain_demo_spawn() -> bool:
    var heights := sim.sample_terrain_patch(
        MOUNTAIN_DEMO_CENTER_EAST_M,
        MOUNTAIN_DEMO_CENTER_NORTH_M,
        MOUNTAIN_DEMO_SAMPLE_SPACING_M,
        MOUNTAIN_DEMO_RESOLUTION
    )
    if heights.size() != MOUNTAIN_DEMO_RESOLUTION * MOUNTAIN_DEMO_RESOLUTION:
        return false

    var max_index := 0
    for i in range(1, heights.size()):
        if float(heights[i]) > float(heights[max_index]):
            max_index = i

    var max_x := max_index % MOUNTAIN_DEMO_RESOLUTION
    var max_z := floori(float(max_index) / float(MOUNTAIN_DEMO_RESOLUTION))
    var target_height_m := float(heights[max_index])
    var view_index := -1
    var best_elevation_angle := -INF
    for i in range(heights.size()):
        var x := i % MOUNTAIN_DEMO_RESOLUTION
        var z := floori(float(i) / float(MOUNTAIN_DEMO_RESOLUTION))
        var dx_m := float(x - max_x) * MOUNTAIN_DEMO_SAMPLE_SPACING_M
        var dz_m := float(z - max_z) * MOUNTAIN_DEMO_SAMPLE_SPACING_M
        var distance_m := Vector2(dx_m, dz_m).length()
        var height_m := float(heights[i])
        if (
            distance_m < MOUNTAIN_VIEW_MIN_DISTANCE_M
            or distance_m > MOUNTAIN_VIEW_MAX_DISTANCE_M
            or height_m < 0.0
        ):
            continue
        var elevation_angle := atan2(
            target_height_m - height_m,
            maxf(distance_m, 1.0)
        )
        if elevation_angle > best_elevation_angle:
            best_elevation_angle = elevation_angle
            view_index = i
    if view_index < 0 or best_elevation_angle <= 0.0:
        return false

    var half := 0.5 * float(MOUNTAIN_DEMO_RESOLUTION - 1)
    var view_x := view_index % MOUNTAIN_DEMO_RESOLUTION
    var view_z := floori(float(view_index) / float(MOUNTAIN_DEMO_RESOLUTION))
    origin_east_m = MOUNTAIN_DEMO_CENTER_EAST_M + (
        float(view_x) - half
    ) * MOUNTAIN_DEMO_SAMPLE_SPACING_M
    origin_north_m = MOUNTAIN_DEMO_CENTER_NORTH_M + (
        float(view_z) - half
    ) * MOUNTAIN_DEMO_SAMPLE_SPACING_M
    mountain_target_east_m = MOUNTAIN_DEMO_CENTER_EAST_M + (
        float(max_x) - half
    ) * MOUNTAIN_DEMO_SAMPLE_SPACING_M
    mountain_target_north_m = MOUNTAIN_DEMO_CENTER_NORTH_M + (
        float(max_z) - half
    ) * MOUNTAIN_DEMO_SAMPLE_SPACING_M
    return true

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
    var surface := sim.sample_surface_visual_patch(
        center_east_m,
        center_north_m,
        SAMPLE_SPACING_M,
        CHUNK_RESOLUTION
    )
    if (
        heights.size() != CHUNK_RESOLUTION * CHUNK_RESOLUTION
        or !_surface_packet_valid(surface, heights.size())
    ):
        status.text = tr("HUD_STATUS_ERROR") % sim.get_last_error()
        return

    var shape := HeightMapShape3D.new()
    shape.map_width = CHUNK_RESOLUTION
    shape.map_depth = CHUNK_RESOLUTION
    var collision_heights := PackedFloat32Array()
    collision_heights.resize(heights.size())
    for z in range(CHUNK_RESOLUTION):
        for x in range(CHUNK_RESOLUTION):
            var scene_index := z * CHUNK_RESOLUTION + x
            var source_index := (
                (CHUNK_RESOLUTION - 1 - z) * CHUNK_RESOLUTION + x
            )
            collision_heights[scene_index] = (
                heights[source_index] / SAMPLE_SPACING_M
            )
    shape.map_data = collision_heights

    if chunks.has(coord):
        var existing_chunk: Node3D = chunks[coord]
        var existing_mesh := existing_chunk.get_node("Mesh") as MeshInstance3D
        var existing_collision := existing_chunk.get_node("Body/Collision") as CollisionShape3D
        existing_mesh.mesh = _build_chunk_mesh(heights, surface)
        existing_collision.shape = shape
        existing_chunk.position = _chunk_local_position(coord)
        _update_chunk_vegetation(existing_chunk, coord, heights, surface)
    else:
        var chunk := Node3D.new()
        chunk.name = "Chunk_%d_%d" % [coord.x, coord.y]
        chunk.position = _chunk_local_position(coord)
        terrain_root.add_child(chunk)

        var mesh_instance := MeshInstance3D.new()
        mesh_instance.name = "Mesh"
        mesh_instance.mesh = _build_chunk_mesh(heights, surface)
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
        _update_chunk_vegetation(chunk, coord, heights, surface)

        chunks[coord] = chunk

    dirty_chunks.erase(coord)

func _observe_surface_revision() -> void:
    var next_revision := sim.get_surface_revision()
    if next_revision == pending_surface_revision:
        return
    pending_surface_revision = next_revision
    distant_terrain.call("set_surface_revision", next_revision)

func _refresh_surface_revision(delta: float) -> void:
    if pending_surface_revision == surface_revision:
        surface_refresh_elapsed = 0.0
        return
    surface_refresh_elapsed += delta
    if surface_refresh_elapsed < SURFACE_REFRESH_INTERVAL_S:
        return

    surface_refresh_elapsed = fmod(
        surface_refresh_elapsed,
        SURFACE_REFRESH_INTERVAL_S
    )
    surface_revision = pending_surface_revision
    for key in chunks.keys():
        var coord: Vector2i = key
        dirty_chunks[coord] = true
    _queue_visible_chunks(current_chunk)

func _refresh_terrain_revision(
    old_origin_height: float,
    old_ground_height: float,
    was_grounded: bool
) -> void:
    var next_revision := sim.get_terrain_revision()
    if next_revision == terrain_revision:
        return

    terrain_revision = next_revision
    distant_terrain.call("set_terrain_revision", terrain_revision)
    var east_m := origin_east_m + float(player.position.x)
    var north_m := origin_north_m - float(player.position.z)
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

func _build_chunk_mesh(
    heights: PackedFloat32Array,
    surface: Dictionary
) -> ArrayMesh:
    var vertex_count := CHUNK_RESOLUTION * CHUNK_RESOLUTION
    var vertices := PackedVector3Array()
    var normals := PackedVector3Array()
    var colors := PackedColorArray()
    vertices.resize(vertex_count)
    normals.resize(vertex_count)
    colors.resize(vertex_count)

    var grass: PackedFloat32Array = surface["grass_density_kg_m2"]
    var shrub: PackedFloat32Array = surface["shrub_density_kg_m2"]
    var tree: PackedFloat32Array = surface["tree_density_kg_m2"]
    var snow: PackedFloat32Array = surface["snow_cover_fraction"]
    var flooded: PackedFloat32Array = surface["flooded_fraction"]
    var fire_active: PackedFloat32Array = surface["fire_active_fraction"]
    var fire_burned: PackedFloat32Array = surface["fire_burned_fraction"]

    var half := CHUNK_SIZE_M * 0.5
    for z in range(CHUNK_RESOLUTION):
        for x in range(CHUNK_RESOLUTION):
            var i := z * CHUNK_RESOLUTION + x
            var source_z := CHUNK_RESOLUTION - 1 - z
            var source_i := source_z * CHUNK_RESOLUTION + x
            var height := float(heights[source_i])
            vertices[i] = Vector3(
                float(x) * SAMPLE_SPACING_M - half,
                height,
                float(z) * SAMPLE_SPACING_M - half
            )

            var left := float(
                heights[source_z * CHUNK_RESOLUTION + maxi(x - 1, 0)]
            )
            var right_h := float(
                heights[source_z * CHUNK_RESOLUTION + mini(
                    x + 1,
                    CHUNK_RESOLUTION - 1
                )]
            )
            var north_z := mini(source_z + 1, CHUNK_RESOLUTION - 1)
            var south_z := maxi(source_z - 1, 0)
            var north_h := float(heights[north_z * CHUNK_RESOLUTION + x])
            var south_h := float(heights[south_z * CHUNK_RESOLUTION + x])
            normals[i] = Vector3(
                left - right_h,
                2.0 * SAMPLE_SPACING_M,
                north_h - south_h
            ).normalized()

            colors[i] = SurfaceVisual.terrain_color(
                height,
                float(grass[source_i]),
                float(shrub[source_i]),
                float(tree[source_i]),
                float(snow[source_i]),
                float(flooded[source_i]),
                float(fire_active[source_i]),
                float(fire_burned[source_i]),
                1.0 - clampf(normals[i].y, 0.0, 1.0)
            )

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

func _surface_packet_valid(surface: Dictionary, expected: int) -> bool:
    for key in [
        "grass_density_kg_m2",
        "shrub_density_kg_m2",
        "tree_density_kg_m2",
        "snow_cover_fraction",
        "flooded_fraction",
        "fire_active_fraction",
        "fire_burned_fraction",
    ]:
        var values: PackedFloat32Array = surface.get(key, PackedFloat32Array())
        if values.size() != expected:
            return false
    return true

func _initialize_vegetation_meshes() -> void:
    var trunk_material := StandardMaterial3D.new()
    trunk_material.albedo_color = Color(0.25, 0.14, 0.07)
    trunk_material.roughness = 0.95

    var leaf_material := StandardMaterial3D.new()
    leaf_material.albedo_color = Color(0.10, 0.27, 0.09)
    leaf_material.roughness = 0.95

    var trunk := CylinderMesh.new()
    trunk.top_radius = 0.30
    trunk.bottom_radius = 0.42
    trunk.height = 3.0
    trunk.radial_segments = 6
    trunk.rings = 1

    var crown := SphereMesh.new()
    crown.radius = 2.15
    crown.height = 4.5
    crown.radial_segments = 8
    crown.rings = 4

    tree_mesh = _combine_primitive_surfaces([
        {
            "mesh": trunk,
            "transform": Transform3D(
                Basis.IDENTITY,
                Vector3(0.0, -2.0, 0.0)
            ),
            "material": trunk_material,
        },
        {
            "mesh": crown,
            "transform": Transform3D(
                Basis.IDENTITY,
                Vector3(0.0, 1.0, 0.0)
            ),
            "material": leaf_material,
        },
    ])

    var shrub_material := StandardMaterial3D.new()
    shrub_material.albedo_color = Color(0.18, 0.34, 0.13)
    shrub_material.roughness = 0.95

    var shrub := SphereMesh.new()
    shrub.radius = 1.0
    shrub.height = SHRUB_VISUAL_HEIGHT_M
    shrub.radial_segments = 7
    shrub.rings = 3
    shrub_mesh = _combine_primitive_surfaces([
        {
            "mesh": shrub,
            "transform": Transform3D.IDENTITY,
            "material": shrub_material,
        },
    ])

func _combine_primitive_surfaces(parts: Array) -> ArrayMesh:
    var combined := ArrayMesh.new()
    for part_variant in parts:
        var part: Dictionary = part_variant
        var primitive := part["mesh"] as PrimitiveMesh
        var part_transform: Transform3D = part["transform"]
        var part_material: Material = part["material"]
        var surface_tool := SurfaceTool.new()
        surface_tool.begin(Mesh.PRIMITIVE_TRIANGLES)
        surface_tool.append_from(
            primitive,
            0,
            part_transform
        )
        surface_tool.commit(combined)
        combined.surface_set_material(
            combined.get_surface_count() - 1,
            part_material
        )
    return combined

func _update_chunk_vegetation(
    chunk: Node3D,
    coord: Vector2i,
    heights: PackedFloat32Array,
    surface: Dictionary
) -> void:
    var trees := chunk.get_node_or_null("Trees") as MultiMeshInstance3D
    if trees == null:
        trees = MultiMeshInstance3D.new()
        trees.name = "Trees"
        trees.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        chunk.add_child(trees)
    var tree_density: PackedFloat32Array = surface["tree_density_kg_m2"]
    trees.multimesh = _build_vegetation_multimesh(
        coord,
        heights,
        tree_density,
        TREE_CANDIDATES_PER_CHUNK,
        SurfaceVisual.TREE_SATURATION_KG_M2,
        tree_mesh,
        TREE_VISUAL_HEIGHT_M,
        101
    )

    var shrubs := chunk.get_node_or_null("Shrubs") as MultiMeshInstance3D
    if shrubs == null:
        shrubs = MultiMeshInstance3D.new()
        shrubs.name = "Shrubs"
        shrubs.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        chunk.add_child(shrubs)
    var shrub_density: PackedFloat32Array = surface["shrub_density_kg_m2"]
    shrubs.multimesh = _build_vegetation_multimesh(
        coord,
        heights,
        shrub_density,
        SHRUB_CANDIDATES_PER_CHUNK,
        SurfaceVisual.SHRUB_SATURATION_KG_M2,
        shrub_mesh,
        SHRUB_VISUAL_HEIGHT_M,
        211
    )

func _build_vegetation_multimesh(
    coord: Vector2i,
    heights: PackedFloat32Array,
    density: PackedFloat32Array,
    candidate_count: int,
    saturation_kg_m2: float,
    visual_mesh: Mesh,
    visual_height_m: float,
    salt: int
) -> MultiMesh:
    var transforms: Array[Transform3D] = []
    var half := CHUNK_SIZE_M * 0.5
    for candidate in range(candidate_count):
        var x_unit := _candidate_unit(coord, candidate, salt)
        var z_unit := _candidate_unit(coord, candidate, salt + 1)
        var local_x := (x_unit - 0.5) * CHUNK_SIZE_M
        var local_z := (z_unit - 0.5) * CHUNK_SIZE_M
        var grid_x := clampi(
            roundi((local_x + half) / SAMPLE_SPACING_M),
            0,
            CHUNK_RESOLUTION - 1
        )
        var grid_z := clampi(
            roundi((local_z + half) / SAMPLE_SPACING_M),
            0,
            CHUNK_RESOLUTION - 1
        )
        var source_z := CHUNK_RESOLUTION - 1 - grid_z
        var index := source_z * CHUNK_RESOLUTION + grid_x
        var height_m := float(heights[index])
        if height_m < 0.0:
            continue

        var cover := SurfaceVisual.cover_from_density(
            float(density[index]),
            saturation_kg_m2
        )
        if _candidate_unit(coord, candidate, salt + 2) >= cover:
            continue

        var scale := 0.65 + 0.70 * _candidate_unit(
            coord,
            candidate,
            salt + 3
        )
        var yaw := TAU * _candidate_unit(coord, candidate, salt + 4)
        var basis := Basis(Vector3.UP, yaw).scaled(Vector3.ONE * scale)
        transforms.push_back(
            Transform3D(
                basis,
                Vector3(
                    local_x,
                    height_m + 0.5 * visual_height_m * scale,
                    local_z
                )
            )
        )

    var multimesh := MultiMesh.new()
    multimesh.transform_format = MultiMesh.TRANSFORM_3D
    multimesh.mesh = visual_mesh
    multimesh.instance_count = transforms.size()
    for i in range(transforms.size()):
        multimesh.set_instance_transform(i, transforms[i])
    return multimesh

func _candidate_unit(
    coord: Vector2i,
    candidate: int,
    salt: int
) -> float:
    var hash_value := (
        "%d:%d:%d:%d" % [coord.x, coord.y, candidate, salt]
    ).hash()
    return float(posmod(hash_value, 1000003)) / 1000003.0

func _chunk_local_position(coord: Vector2i) -> Vector3:
    return Vector3(
        float(coord.x) * CHUNK_SIZE_M - origin_east_m,
        -origin_height_m,
        origin_north_m - float(coord.y) * CHUNK_SIZE_M
    )

func _maybe_shift_origin() -> void:
    if absf(player.position.x) < ORIGIN_SHIFT_THRESHOLD_M and absf(player.position.z) < ORIGIN_SHIFT_THRESHOLD_M:
        return

    var old_origin_height := origin_height_m
    origin_east_m += float(player.position.x)
    origin_north_m -= float(player.position.z)
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
    var north_m := origin_north_m - float(player.position.z)
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

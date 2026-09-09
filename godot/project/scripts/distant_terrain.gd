extends Node3D

const SurfaceVisual = preload("res://scripts/surface_visual.gd")

# Visual-only spherical terrain. Physics stays on the existing fine local chunks.
# Each coarser level doubles sample spacing and renders only the ring not covered
# by the next-finer level. Fine outer rows morph onto the coarser grid so the
# independently generated meshes meet without T-junction cracks.

const LOD_RESOLUTION := 65
const LOD_SPACINGS_M := [
    64.0,
    128.0,
    256.0,
    512.0,
    1024.0,
    2048.0,
    4096.0,
    8192.0,
    16384.0,
]
const TERRAIN_INNER_HALF_M := 640.0
const WALK_RECENTER_DISTANCE_M := 256.0
const SURVEY_RECENTER_DISTANCE_M := 65536.0
const REVISION_REFRESH_INTERVAL_S := 5.0

var sim: WorldSimulationNode
var terrain_material: StandardMaterial3D
var ocean_material: StandardMaterial3D
var level_nodes: Array[Node3D] = []
var level_positions: Array[PackedVector3Array] = []
var level_normals: Array[PackedVector3Array] = []
var level_sea_positions: Array[PackedVector3Array] = []
var pending_levels: Array[int] = []

var origin_east_m := 0.0
var origin_north_m := 0.0
var origin_height_m := 0.0
var target_center_east_m := 0.0
var target_center_north_m := 0.0
var queued_center_east_m := 0.0
var queued_center_north_m := 0.0
var terrain_revision := 0
var surface_revision := 0
var revision_dirty := false
var revision_elapsed_s := 0.0
var initialized := false

func initialize(
    sim_node: WorldSimulationNode,
    initial_origin_east_m: float,
    initial_origin_north_m: float,
    initial_origin_height_m: float
) -> void:
    sim = sim_node
    origin_east_m = initial_origin_east_m
    origin_north_m = initial_origin_north_m
    origin_height_m = initial_origin_height_m
    target_center_east_m = initial_origin_east_m
    target_center_north_m = initial_origin_north_m
    queued_center_east_m = target_center_east_m
    queued_center_north_m = target_center_north_m
    terrain_revision = sim.get_terrain_revision()
    surface_revision = sim.get_surface_revision()

    terrain_material = StandardMaterial3D.new()
    terrain_material.vertex_color_use_as_albedo = true
    terrain_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    terrain_material.roughness = 0.95

    ocean_material = StandardMaterial3D.new()
    ocean_material.albedo_color = Color(0.035, 0.16, 0.27, 1.0)
    ocean_material.roughness = 0.22
    ocean_material.metallic = 0.06

    level_positions.resize(LOD_SPACINGS_M.size())
    level_normals.resize(LOD_SPACINGS_M.size())
    level_sea_positions.resize(LOD_SPACINGS_M.size())
    for level in range(LOD_SPACINGS_M.size()):
        level_positions[level] = PackedVector3Array()
        level_normals[level] = PackedVector3Array()
        level_sea_positions[level] = PackedVector3Array()
        var level_node := Node3D.new()
        level_node.name = "Lod%d" % level
        add_child(level_node)

        var terrain_mesh := MeshInstance3D.new()
        terrain_mesh.name = "Terrain"
        terrain_mesh.material_override = terrain_material
        terrain_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        level_node.add_child(terrain_mesh)

        var ocean_mesh := MeshInstance3D.new()
        ocean_mesh.name = "Ocean"
        ocean_mesh.material_override = ocean_material
        ocean_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        level_node.add_child(ocean_mesh)

        level_nodes.push_back(level_node)

    initialized = true
    _queue_all_levels()

func set_terrain_revision(revision: int) -> void:
    if !initialized or revision == terrain_revision:
        return
    terrain_revision = revision
    revision_dirty = true

func set_surface_revision(revision: int) -> void:
    if !initialized or revision == surface_revision:
        return
    surface_revision = revision
    revision_dirty = true

func set_view_state(
    center_east_m: float,
    center_north_m: float,
    next_origin_east_m: float,
    next_origin_north_m: float,
    next_origin_height_m: float,
    survey_flight_enabled: bool
) -> void:
    if !initialized:
        return

    var origin_changed := (
        next_origin_east_m != origin_east_m
        or next_origin_north_m != origin_north_m
        or next_origin_height_m != origin_height_m
    )
    if origin_changed:
        # Keep already-built geometry approximately world-stationary while a new
        # spherical frame is rebuilt level by level. The exact curvature/basis is
        # restored by sample_terrain_visual_patch() on each replacement mesh.
        var local_shift := Vector3(
            origin_east_m - next_origin_east_m,
            origin_height_m - next_origin_height_m,
            next_origin_north_m - origin_north_m
        )
        for level_node in level_nodes:
            level_node.position += local_shift
        origin_east_m = next_origin_east_m
        origin_north_m = next_origin_north_m
        origin_height_m = next_origin_height_m
        if !survey_flight_enabled:
            queued_center_east_m = center_east_m
            queued_center_north_m = center_north_m
            _queue_all_levels()

    target_center_east_m = center_east_m
    target_center_north_m = center_north_m
    var threshold := (
        SURVEY_RECENTER_DISTANCE_M
        if survey_flight_enabled
        else WALK_RECENTER_DISTANCE_M
    )
    var dx := target_center_east_m - queued_center_east_m
    var dz := target_center_north_m - queued_center_north_m
    if dx * dx + dz * dz >= threshold * threshold:
        queued_center_east_m = target_center_east_m
        queued_center_north_m = target_center_north_m
        _queue_all_levels()

func _process(delta: float) -> void:
    if !initialized:
        return

    if revision_dirty:
        revision_elapsed_s += delta
        if revision_elapsed_s >= REVISION_REFRESH_INTERVAL_S and pending_levels.is_empty():
            revision_dirty = false
            revision_elapsed_s = 0.0
            _queue_all_levels()

    if pending_levels.is_empty():
        return

    var level: int = pending_levels.pop_front()
    _rebuild_level(level)

func _queue_all_levels() -> void:
    pending_levels.clear()
    # Coarse-to-fine guarantees that every fine outer transition can morph onto
    # the matching newly sampled coarser ring.
    for level in range(LOD_SPACINGS_M.size() - 1, -1, -1):
        pending_levels.push_back(level)

func _rebuild_level(level: int) -> void:
    var spacing_m := float(LOD_SPACINGS_M[level])
    var packet := sim.sample_terrain_visual_patch(
        queued_center_east_m,
        queued_center_north_m,
        spacing_m,
        LOD_RESOLUTION,
        origin_east_m,
        origin_north_m,
        origin_height_m
    )
    var positions: PackedVector3Array = packet.get("positions", PackedVector3Array())
    var sea_positions: PackedVector3Array = packet.get("sea_positions", PackedVector3Array())
    var heights: PackedFloat32Array = packet.get("heights", PackedFloat32Array())
    var surface := sim.sample_surface_visual_patch(
        queued_center_east_m,
        queued_center_north_m,
        spacing_m,
        LOD_RESOLUTION
    )
    var expected := LOD_RESOLUTION * LOD_RESOLUTION
    if (
        positions.size() != expected
        or sea_positions.size() != expected
        or heights.size() != expected
        or !_surface_packet_valid(surface, expected)
    ):
        push_error("Distant terrain LOD %d sampling failed: %s" % [level, sim.get_last_error()])
        return

    if level + 1 < LOD_SPACINGS_M.size():
        var coarse_positions: PackedVector3Array = level_positions[level + 1]
        var coarse_sea_positions: PackedVector3Array = level_sea_positions[level + 1]
        if coarse_positions.size() == expected:
            positions = _morph_outer_transition(positions, coarse_positions)
        if coarse_sea_positions.size() == expected:
            sea_positions = _morph_outer_transition(sea_positions, coarse_sea_positions)

    var normals := _compute_normals(positions)
    if level + 1 < LOD_SPACINGS_M.size():
        var coarse_normals: PackedVector3Array = level_normals[level + 1]
        if coarse_normals.size() == expected:
            normals = _morph_outer_normals(normals, coarse_normals)

    level_positions[level] = positions
    level_normals[level] = normals
    level_sea_positions[level] = sea_positions

    var level_node: Node3D = level_nodes[level]
    level_node.position = Vector3.ZERO
    var terrain_instance := level_node.get_node("Terrain") as MeshInstance3D
    var ocean_instance := level_node.get_node("Ocean") as MeshInstance3D
    terrain_instance.mesh = _build_mesh(
        positions,
        heights,
        surface,
        spacing_m,
        _terrain_inner_half_m(level),
        true,
        normals
    )
    ocean_instance.mesh = _build_mesh(
        sea_positions,
        PackedFloat32Array(),
        {},
        spacing_m,
        _ocean_inner_half_m(level),
        false
    )

func _terrain_inner_half_m(level: int) -> float:
    if level == 0:
        return TERRAIN_INNER_HALF_M
    return _outer_half_m(level - 1)

func _ocean_inner_half_m(level: int) -> float:
    if level == 0:
        return 0.0
    return _outer_half_m(level - 1)

func _outer_half_m(level: int) -> float:
    return float(LOD_SPACINGS_M[level]) * float(LOD_RESOLUTION - 1) * 0.5

func _morph_outer_transition(
    positions: PackedVector3Array,
    coarse_positions: PackedVector3Array
) -> PackedVector3Array:
    var center := floori(float(LOD_RESOLUTION - 1) * 0.5)
    for z in range(LOD_RESOLUTION):
        for x in range(LOD_RESOLUTION):
            var ring := maxi(absi(x - center), absi(z - center))
            if ring < center - 1:
                continue
            var blend := 1.0 if ring == center else 0.5
            var coarse_x := float(center) + float(x - center) * 0.5
            var coarse_z := float(center) + float(z - center) * 0.5
            var coarse_position := _sample_grid_bilinear(
                coarse_positions,
                coarse_x,
                coarse_z
            )
            var index := z * LOD_RESOLUTION + x
            positions[index] = positions[index].lerp(coarse_position, blend)
    return positions

func _compute_normals(
    positions: PackedVector3Array
) -> PackedVector3Array:
    var normals := PackedVector3Array()
    normals.resize(positions.size())
    for z in range(LOD_RESOLUTION):
        for x in range(LOD_RESOLUTION):
            var index := z * LOD_RESOLUTION + x
            var left := positions[z * LOD_RESOLUTION + maxi(x - 1, 0)]
            var right := positions[z * LOD_RESOLUTION + mini(x + 1, LOD_RESOLUTION - 1)]
            var down := positions[maxi(z - 1, 0) * LOD_RESOLUTION + x]
            var up := positions[mini(z + 1, LOD_RESOLUTION - 1) * LOD_RESOLUTION + x]
            normals[index] = (right - left).cross(up - down).normalized()
    return normals

func _morph_outer_normals(
    normals: PackedVector3Array,
    coarse_normals: PackedVector3Array
) -> PackedVector3Array:
    var center := floori(float(LOD_RESOLUTION - 1) * 0.5)
    for z in range(LOD_RESOLUTION):
        for x in range(LOD_RESOLUTION):
            var ring := maxi(absi(x - center), absi(z - center))
            if ring < center - 1:
                continue
            var blend := 1.0 if ring == center else 0.5
            var coarse_x := float(center) + float(x - center) * 0.5
            var coarse_z := float(center) + float(z - center) * 0.5
            var coarse_normal := _sample_normal_bilinear(
                coarse_normals,
                coarse_x,
                coarse_z
            )
            var index := z * LOD_RESOLUTION + x
            normals[index] = normals[index].lerp(
                coarse_normal,
                blend
            ).normalized()
    return normals

func _sample_normal_bilinear(
    normals: PackedVector3Array,
    x: float,
    z: float
) -> Vector3:
    var x0 := clampi(floori(x), 0, LOD_RESOLUTION - 1)
    var z0 := clampi(floori(z), 0, LOD_RESOLUTION - 1)
    var x1 := mini(x0 + 1, LOD_RESOLUTION - 1)
    var z1 := mini(z0 + 1, LOD_RESOLUTION - 1)
    var tx := clampf(x - float(x0), 0.0, 1.0)
    var tz := clampf(z - float(z0), 0.0, 1.0)
    var a := normals[z0 * LOD_RESOLUTION + x0].lerp(
        normals[z0 * LOD_RESOLUTION + x1],
        tx
    )
    var b := normals[z1 * LOD_RESOLUTION + x0].lerp(
        normals[z1 * LOD_RESOLUTION + x1],
        tx
    )
    return a.lerp(b, tz).normalized()

func _sample_grid_bilinear(
    positions: PackedVector3Array,
    x: float,
    z: float
) -> Vector3:
    var x0 := clampi(floori(x), 0, LOD_RESOLUTION - 1)
    var z0 := clampi(floori(z), 0, LOD_RESOLUTION - 1)
    var x1 := mini(x0 + 1, LOD_RESOLUTION - 1)
    var z1 := mini(z0 + 1, LOD_RESOLUTION - 1)
    var tx := clampf(x - float(x0), 0.0, 1.0)
    var tz := clampf(z - float(z0), 0.0, 1.0)
    var a := positions[z0 * LOD_RESOLUTION + x0].lerp(
        positions[z0 * LOD_RESOLUTION + x1],
        tx
    )
    var b := positions[z1 * LOD_RESOLUTION + x0].lerp(
        positions[z1 * LOD_RESOLUTION + x1],
        tx
    )
    return a.lerp(b, tz)

func _build_mesh(
    positions: PackedVector3Array,
    heights: PackedFloat32Array,
    surface: Dictionary,
    spacing_m: float,
    inner_half_m: float,
    use_elevation_colors: bool,
    provided_normals: PackedVector3Array = PackedVector3Array()
) -> ArrayMesh:
    var normals := provided_normals
    if normals.size() != positions.size():
        normals = _compute_normals(positions)
    var colors := PackedColorArray()
    var grass := PackedFloat32Array()
    var shrub := PackedFloat32Array()
    var tree := PackedFloat32Array()
    var snow := PackedFloat32Array()
    var flooded := PackedFloat32Array()
    var fire_active := PackedFloat32Array()
    var fire_burned := PackedFloat32Array()
    if use_elevation_colors:
        colors.resize(positions.size())
        grass = surface["grass_density_kg_m2"]
        shrub = surface["shrub_density_kg_m2"]
        tree = surface["tree_density_kg_m2"]
        snow = surface["snow_cover_fraction"]
        flooded = surface["flooded_fraction"]
        fire_active = surface["fire_active_fraction"]
        fire_burned = surface["fire_burned_fraction"]

    var half_cells := float(LOD_RESOLUTION - 1) * 0.5
    for z in range(LOD_RESOLUTION):
        for x in range(LOD_RESOLUTION):
            var index := z * LOD_RESOLUTION + x
            if use_elevation_colors:
                var east_m := queued_center_east_m + (
                    float(x) - half_cells
                ) * spacing_m
                var north_m := queued_center_north_m + (
                    float(z) - half_cells
                ) * spacing_m
                colors[index] = SurfaceVisual.terrain_color(
                    float(heights[index]),
                    float(grass[index]),
                    float(shrub[index]),
                    float(tree[index]),
                    float(snow[index]),
                    float(flooded[index]),
                    float(fire_active[index]),
                    float(fire_burned[index]),
                    Vector2(normals[index].x, normals[index].z).length()
                    / maxf(normals[index].y, 0.001),
                    SurfaceVisual.relief_light(normals[index]),
                    SurfaceVisual.terrain_detail(east_m, north_m)
                )

    var indices := PackedInt32Array()
    for z in range(LOD_RESOLUTION - 1):
        for x in range(LOD_RESOLUTION - 1):
            var mid_x := (float(x) + 0.5 - half_cells) * spacing_m
            var mid_z := (float(z) + 0.5 - half_cells) * spacing_m
            if (
                inner_half_m > 0.0
                and absf(mid_x) < inner_half_m
                and absf(mid_z) < inner_half_m
            ):
                continue
            var i0 := z * LOD_RESOLUTION + x
            var i1 := i0 + 1
            var i2 := i0 + LOD_RESOLUTION
            var i3 := i2 + 1
            # Godot 4.7 treats clockwise winding as front-facing.
            indices.push_back(i0)
            indices.push_back(i2)
            indices.push_back(i1)
            indices.push_back(i1)
            indices.push_back(i2)
            indices.push_back(i3)

    var arrays := []
    arrays.resize(Mesh.ARRAY_MAX)
    arrays[Mesh.ARRAY_VERTEX] = positions
    arrays[Mesh.ARRAY_NORMAL] = normals
    if use_elevation_colors:
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

extends SceneTree

const SurfaceVisual = preload("res://scripts/surface_visual.gd")

# Catches the inverted-winding failure: Godot 4.7 uses clockwise front faces,
# so a +Y right-hand winding was culled and the terrain collapsed to a noisy
# horizon line. https://docs.godotengine.org/en/4.7/classes/class_arraymesh.html

var scene: Node

func _initialize() -> void:
    var packed := load("res://main.tscn")
    scene = packed.instantiate()
    root.add_child(scene)

func _process(_delta: float) -> bool:
    if Engine.get_process_frames() < 2:
        return false

    var terrain := root.get_node_or_null("WorldViewer/Terrain")
    if terrain == null:
        push_error("Terrain node missing from main.tscn")
        quit(1)
        return true

    var spawn_chunk: Vector2i = scene.get("current_chunk")
    var spawn_chunk_name := "Chunk_%d_%d" % [spawn_chunk.x, spawn_chunk.y]
    var mesh_instance := terrain.get_node_or_null(
        "%s/Mesh" % spawn_chunk_name
    ) as MeshInstance3D
    if mesh_instance == null:
        push_error("No terrain MeshInstance3D was created")
        quit(2)
        return true

    var mesh := mesh_instance.mesh as ArrayMesh
    if mesh == null or mesh.get_surface_count() < 1:
        push_error("Terrain mesh surface is missing")
        quit(3)
        return true

    var arrays := mesh.surface_get_arrays(0)
    var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
    var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
    var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
    var colors: PackedColorArray = arrays[Mesh.ARRAY_COLOR]
    if verts.size() < 3 or indices.size() < 6:
        push_error("Terrain mesh arrays are too small")
        quit(4)
        return true
    if colors.size() != verts.size():
        push_error("Near terrain does not carry living-surface vertex colors")
        quit(40)
        return true

    var viewer_sim := root.get_node("WorldViewer/Simulation") as WorldSimulationNode
    var spawn_east_m := float(scene.get("origin_east_m"))
    var spawn_north_m := float(scene.get("origin_north_m"))
    var mountain_target_east_m := float(scene.get("mountain_target_east_m"))
    var mountain_target_north_m := float(scene.get("mountain_target_north_m"))
    var tree_values := viewer_sim.get_field_values("ecology.tree_carbon_kg")
    if tree_values.is_empty():
        push_error("Walker did not initialize the authoritative ecology modules")
        quit(41)
        return true

    var surface_packet := viewer_sim.sample_surface_visual_patch(
        spawn_east_m, spawn_north_m, 8.0, 33
    )
    if !_surface_packet_valid(surface_packet, 33 * 33):
        push_error("Living-surface visual packet is missing or invalid")
        quit(42)
        return true

    var chunk_center_east_m := float(spawn_chunk.x) * 256.0
    var chunk_center_north_m := float(spawn_chunk.y) * 256.0
    var normal_probe := viewer_sim.sample_terrain_patch(
        chunk_center_east_m,
        chunk_center_north_m,
        8.0,
        35
    )
    if normal_probe.size() != 35 * 35:
        push_error("Near terrain ghost-border normal probe failed")
        quit(68)
        return true
    var edge_scene_z := 16
    var edge_source_z := 32 - edge_scene_z
    var edge_normal_z := edge_source_z + 1
    var edge_normal_x := 33
    var expected_edge_normal := Vector3(
        float(normal_probe[edge_normal_z * 35 + edge_normal_x - 1])
        - float(normal_probe[edge_normal_z * 35 + edge_normal_x + 1]),
        16.0,
        float(normal_probe[(edge_normal_z + 1) * 35 + edge_normal_x])
        - float(normal_probe[(edge_normal_z - 1) * 35 + edge_normal_x])
    ).normalized()
    var rendered_edge_normal := normals[edge_scene_z * 33 + 32]
    if rendered_edge_normal.dot(expected_edge_normal) < 0.9999:
        push_error("Near chunk edge normal no longer uses the ghost-border central derivative")
        quit(68)
        return true

    var mountain_probe := viewer_sim.sample_terrain_patch(
        5_573_000.0,
        -1_800_300.0,
        625.0,
        65
    )
    if mountain_probe.size() != 65 * 65:
        push_error("Mountain diagnostic patch failed")
        quit(52)
        return true
    var mountain_min := float(mountain_probe[0])
    var mountain_max := mountain_min
    var mountain_min_index := 0
    var mountain_max_index := 0
    for i in range(1, mountain_probe.size()):
        var value := float(mountain_probe[i])
        if value < mountain_min:
            mountain_min = value
            mountain_min_index = i
        if value > mountain_max:
            mountain_max = value
            mountain_max_index = i
    var mountain_center := float(mountain_probe[32 * 65 + 32])
    var mountain_span := mountain_max - mountain_min
    var mountain_prominence := mountain_center - mountain_min
    if mountain_span < 1_000.0 or mountain_prominence < 800.0:
        push_error(
            "Reported mountain regressed to broad highland: span=%.2f prominence=%.2f"
            % [mountain_span, mountain_prominence]
        )
        quit(53)
        return true

    var mountain_half := 32.0
    var view_x := roundi(
        (spawn_east_m - 5_573_000.0) / 625.0 + mountain_half
    )
    var view_z := roundi(
        (spawn_north_m + 1_800_300.0) / 625.0 + mountain_half
    )
    var target_x := roundi(
        (mountain_target_east_m - 5_573_000.0) / 625.0 + mountain_half
    )
    var target_z := roundi(
        (mountain_target_north_m + 1_800_300.0) / 625.0 + mountain_half
    )
    if (
        view_x < 0 or view_x >= 65
        or view_z < 0 or view_z >= 65
        or target_x < 0 or target_x >= 65
        or target_z < 0 or target_z >= 65
    ):
        push_error("Mountain demonstration viewpoint escaped the diagnostic patch")
        quit(54)
        return true

    var view_index := view_z * 65 + view_x
    var target_index := target_z * 65 + target_x
    var view_height_m := float(mountain_probe[view_index])
    var target_height_m := float(mountain_probe[target_index])
    var mountain_view_distance_m := Vector2(
        float(target_x - view_x) * 625.0,
        float(target_z - view_z) * 625.0
    ).length()
    var height_rise_m := target_height_m - view_height_m
    var best_elevation_angle := atan2(
        height_rise_m,
        maxf(mountain_view_distance_m, 1.0)
    )
    var best_skyline_margin := float(scene.call(
        "_mountain_skyline_margin",
        mountain_probe,
        Vector2(float(view_x), float(view_z)),
        Vector2(float(target_x), float(target_z))
    ))

    if mountain_view_distance_m < 3_000.0 or mountain_view_distance_m > 7_100.0:
        push_error(
            "Mountain viewpoint distance escaped the presentation range: %.2f km"
            % (mountain_view_distance_m / 1000.0)
        )
        quit(54)
        return true
    if view_height_m < 0.0 or height_rise_m < 950.0:
        push_error(
            "Mountain viewpoint lacks a dry 950 m summit rise: view=%.1f rise=%.1f"
            % [view_height_m, height_rise_m]
        )
        quit(59)
        return true
    if best_elevation_angle < deg_to_rad(8.0):
        push_error(
            "Mountain viewpoint lacks visible angular relief: %.2f degrees"
            % rad_to_deg(best_elevation_angle)
        )
        quit(59)
        return true
    if best_skyline_margin < 0.0:
        push_error(
            "Fixed mountain summit is hidden by foreground skyline: margin=%.2f degrees"
            % rad_to_deg(best_skyline_margin)
        )
        quit(63)
        return true
    if !bool(scene.call(
        "_mountain_is_local_summit",
        mountain_probe,
        target_x,
        target_z
    )):
        push_error("Mountain target is not a local summit")
        quit(64)
        return true
    if target_height_m - mountain_min < 700.0:
        push_error("Mountain target lost regional prominence")
        quit(65)
        return true

    var trees := terrain.get_node_or_null(
        "%s/Trees" % spawn_chunk_name
    ) as MultiMeshInstance3D
    var shrubs := terrain.get_node_or_null(
        "%s/Shrubs" % spawn_chunk_name
    ) as MultiMeshInstance3D
    if (
        trees == null
        or shrubs == null
        or trees.multimesh == null
        or shrubs.multimesh == null
    ):
        push_error("Near authoritative vegetation MultiMeshes were not created")
        quit(43)
        return true
    if trees.multimesh.mesh == null or trees.multimesh.mesh.get_surface_count() < 2:
        push_error("Tree presentation regressed to a single diagnostic cone surface")
        quit(46)
        return true

    # Godot's right-handed terrain convention is +X east and -Z north.
    # The sampled patch is ordered south-to-north, so its north row must appear
    # on the chunk's negative-Z side.
    var source_heights := viewer_sim.sample_terrain_patch(
        chunk_center_east_m,
        chunk_center_north_m,
        8.0,
        33
    )
    var scene_north_index := 16
    var source_north_index := 32 * 33 + 16
    if absf(verts[scene_north_index].y - float(source_heights[source_north_index])) > 0.01:
        push_error("Near terrain north row is mirrored in scene Z")
        quit(47)
        return true

    var player := root.get_node("WorldViewer/Player") as CharacterBody3D
    var initial_forward := -player.global_transform.basis.z
    var actual_target_direction := Vector2(
        initial_forward.x,
        -initial_forward.z
    ).normalized()
    var expected_target_direction := Vector2(
        mountain_target_east_m - spawn_east_m,
        mountain_target_north_m - spawn_north_m
    ).normalized()
    if actual_target_direction.dot(expected_target_direction) < 0.999:
        push_error("Initial walker heading does not face the sampled mountain peak")
        quit(55)
        return true

    var minimap_overlay := root.get_node(
        "WorldViewer/HUD/MiniMapPanel/Margin/VBox/MapFrame/Inset/Layers/Overlay"
    )
    player.rotation.y = 0.0
    scene.call("_update_minimap_marker")
    var north_heading: Vector2 = minimap_overlay.heading_uv_delta
    if north_heading.y >= 0.0:
        push_error("Zero-yaw camera does not point north/up on minimap")
        quit(48)
        return true
    player.rotation.y = -PI * 0.5
    scene.call("_update_minimap_marker")
    var east_heading: Vector2 = minimap_overlay.heading_uv_delta
    if east_heading.x <= 0.0:
        push_error("Right camera turn does not turn minimap arrow east/right")
        quit(49)
        return true
    player.rotation.y = 0.0

    var visual_patch := viewer_sim.sample_terrain_visual_patch(
        spawn_east_m,
        spawn_north_m,
        64.0,
        3,
        spawn_east_m,
        spawn_north_m,
        viewer_sim.sample_terrain_height(spawn_east_m, spawn_north_m)
    )
    var visual_positions: PackedVector3Array = visual_patch.get(
        "positions",
        PackedVector3Array()
    )
    if visual_positions.size() != 9 or visual_positions[7].z >= visual_positions[4].z:
        push_error("Distant local planet frame does not map projected north to -Z")
        quit(50)
        return true

    var n0 := _face_normal(verts, indices, 0)
    var n1 := _face_normal(verts, indices, 3)
    # Clockwise winding when viewed from +Y has a right-hand normal of -Y.
    # That is the Godot 4.7 front face for a ground plane.
    if n0.y >= 0.0 or n1.y >= 0.0:
        push_error("Terrain winding is not clockwise from +Y (n0.y=%f n1.y=%f)" % [n0.y, n1.y])
        quit(5)
        return true

    var aabb := mesh_instance.global_transform * mesh.get_aabb()
    if aabb.end.y < -2.0 or aabb.position.y > 80.0:
        push_error("Terrain AABB is not near the player origin: %s" % aabb)
        quit(6)
        return true

    var collision := terrain.get_node_or_null(
        "%s/Body/Collision" % spawn_chunk_name
    ) as CollisionShape3D
    if !_mesh_matches_collision(mesh, collision):
        push_error("Initial terrain mesh and collision heights disagree")
        quit(7)
        return true

    var distant_root := root.get_node_or_null("WorldViewer/DistantTerrain")
    if distant_root == null:
        push_error("Spherical distant terrain root was not created")
        quit(11)
        return true

    # Build visual LODs explicitly instead of waiting extra global frames. Waiting
    # here used to advance the viewer simulation nondeterministically, invalidating
    # the existing "first focused step refreshes terrain resources" regression.
    for _i in range(12):
        distant_root.call("_process", 0.0)

    var distant_terrain := root.get_node_or_null("WorldViewer/DistantTerrain/Lod8/Terrain") as MeshInstance3D
    var distant_ocean := root.get_node_or_null("WorldViewer/DistantTerrain/Lod8/Ocean") as MeshInstance3D
    var near_ocean := root.get_node_or_null("WorldViewer/DistantTerrain/Lod0/Ocean") as MeshInstance3D
    var near_distant_terrain := root.get_node_or_null("WorldViewer/DistantTerrain/Lod0/Terrain") as MeshInstance3D
    var next_distant_terrain := root.get_node_or_null("WorldViewer/DistantTerrain/Lod1/Terrain") as MeshInstance3D
    if (
        distant_terrain == null
        or distant_ocean == null
        or near_ocean == null
        or near_distant_terrain == null
        or next_distant_terrain == null
    ):
        push_error("Spherical distant terrain/ocean nodes were not created")
        quit(11)
        return true
    if (
        distant_terrain.mesh == null
        or distant_ocean.mesh == null
        or near_ocean.mesh == null
        or near_distant_terrain.mesh == null
        or next_distant_terrain.mesh == null
    ):
        push_error("Spherical distant terrain/ocean meshes were not built")
        quit(12)
        return true

    var distant_arrays := (distant_terrain.mesh as ArrayMesh).surface_get_arrays(0)
    var distant_verts: PackedVector3Array = distant_arrays[Mesh.ARRAY_VERTEX]
    var distant_colors: PackedColorArray = distant_arrays[Mesh.ARRAY_COLOR]
    var distant_indices: PackedInt32Array = distant_arrays[Mesh.ARRAY_INDEX]
    if distant_colors.size() != distant_verts.size():
        push_error("Distant terrain does not carry living-surface vertex colors")
        quit(44)
        return true
    if (
        distant_indices.size() < 3
        or _face_normal(distant_verts, distant_indices, 0).y >= 0.0
    ):
        push_error("Distant terrain winding is not front-facing after north/-Z mapping")
        quit(51)
        return true

    var fine_lod_arrays := (
        near_distant_terrain.mesh as ArrayMesh
    ).surface_get_arrays(0)
    var coarse_lod_arrays := (
        next_distant_terrain.mesh as ArrayMesh
    ).surface_get_arrays(0)
    var fine_lod_verts: PackedVector3Array = fine_lod_arrays[Mesh.ARRAY_VERTEX]
    var fine_lod_colors: PackedColorArray = fine_lod_arrays[Mesh.ARRAY_COLOR]
    var fine_lod_indices: PackedInt32Array = fine_lod_arrays[Mesh.ARRAY_INDEX]
    var coarse_lod_colors: PackedColorArray = coarse_lod_arrays[Mesh.ARRAY_COLOR]

    var expected_distant_center_east_m := float(spawn_chunk.x) * 256.0
    var expected_distant_center_north_m := float(spawn_chunk.y) * 256.0
    if (
        absf(float(distant_root.get("queued_center_east_m")) - expected_distant_center_east_m) > 0.1
        or absf(float(distant_root.get("queued_center_north_m")) - expected_distant_center_north_m) > 0.1
    ):
        push_error("Walking distant terrain is not aligned to the current near chunk center")
        quit(70)
        return true

    if fine_lod_indices.size() != 32 * 32 * 6:
        push_error("Lod0 regained a central terrain hole that can expose streaming gaps")
        quit(71)
        return true

    var raw_lod0 := viewer_sim.sample_terrain_visual_patch(
        expected_distant_center_east_m,
        expected_distant_center_north_m,
        64.0,
        33,
        spawn_east_m,
        spawn_north_m,
        float(scene.get("origin_height_m"))
    )
    var raw_lod0_verts: PackedVector3Array = raw_lod0.get(
        "positions",
        PackedVector3Array()
    )
    if raw_lod0_verts.size() != 33 * 33:
        push_error("Lod0 underlay reference sampling failed")
        quit(72)
        return true
    var lod0_center_index := 16 * 33 + 16
    var lod0_edge_index := 16 * 33 + 32
    var center_underlay_drop_m := (
        raw_lod0_verts[lod0_center_index].y
        - fine_lod_verts[lod0_center_index].y
    )
    var edge_underlay_delta_m := absf(
        raw_lod0_verts[lod0_edge_index].y
        - fine_lod_verts[lod0_edge_index].y
    )
    if center_underlay_drop_m < 40.0 or edge_underlay_delta_m > 1.0:
        push_error(
            "Lod0 safety underlay no longer stays below near terrain and rejoins at its outer edge: center_drop=%.2f edge_delta=%.2f"
            % [center_underlay_drop_m, edge_underlay_delta_m]
        )
        quit(72)
        return true

    var lod_seam_delta := 0.0
    for pair in [
        Vector2i(16 * 33 + 32, 16 * 33 + 24),
        Vector2i(0 * 33 + 16, 8 * 33 + 16),
    ]:
        var fine_color := fine_lod_colors[pair.x]
        var coarse_color := coarse_lod_colors[pair.y]
        lod_seam_delta = maxf(
            lod_seam_delta,
            absf(fine_color.r - coarse_color.r)
            + absf(fine_color.g - coarse_color.g)
            + absf(fine_color.b - coarse_color.b)
        )
    if lod_seam_delta > 0.025:
        push_error(
            "Adjacent distant LODs disagree in vertex shading at their shared boundary: %.4f"
            % lod_seam_delta
        )
        quit(67)
        return true

    var sea_arrays := (distant_ocean.mesh as ArrayMesh).surface_get_arrays(0)
    var sea_verts: PackedVector3Array = sea_arrays[Mesh.ARRAY_VERTEX]
    var distant_resolution := 33
    var center_index := 16 * distant_resolution + 16
    # Lod8 spacing is 16,384 m. The outer sample remains the existing
    # 262.144 km curvature probe with the restored 33 x 33 performance budget.
    var edge_index := 16 * distant_resolution + 32
    if sea_verts.size() != distant_resolution * distant_resolution:
        push_error("Unexpected distant ocean vertex count: %d" % sea_verts.size())
        quit(13)
        return true
    var sea_drop_m := sea_verts[edge_index].y - sea_verts[center_index].y
    if sea_drop_m > -4000.0:
        push_error("Distant sea surface is not planet-curved: drop=%.2f m" % sea_drop_m)
        quit(14)
        return true

    var camera := root.get_node("WorldViewer/Player/Head/Camera3D") as Camera3D
    var world_environment := root.get_node("WorldViewer/WorldEnvironment") as WorldEnvironment
    var sun := root.get_node("WorldViewer/DirectionalLight3D") as DirectionalLight3D
    if camera.far < 300000.0:
        push_error("Camera far plane does not expose planetary terrain: %.1f" % camera.far)
        quit(15)
        return true
    if camera.fov > 68.0:
        push_error("Walking camera framing is too wide for mountain-scale terrain: %.1f" % camera.fov)
        quit(56)
        return true
    if (
        world_environment.environment.fog_mode != Environment.FOG_MODE_DEPTH
        or world_environment.environment.fog_depth_end < 250000.0
    ):
        push_error("Environment does not use long-range depth fog")
        quit(16)
        return true
    if world_environment.environment.ambient_light_energy > 0.45 or sun.light_energy < 1.2:
        push_error("Terrain lighting regressed to flat ambient-dominated shading")
        quit(57)
        return true
    if sun.shadow_enabled:
        push_error("Nested distant terrain must not use shadow maps that expose LOD seams")
        quit(60)
        return true
    if (
        distant_terrain.cast_shadow != GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        or near_distant_terrain.cast_shadow != GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
    ):
        push_error("Distant terrain shadow casting regressed and can expose clipmap seams")
        quit(61)
        return true

    var near_material := mesh_instance.material_override as BaseMaterial3D
    var distant_material := near_distant_terrain.material_override as BaseMaterial3D
    if (
        near_material == null
        or distant_material == null
        or near_material.shading_mode != BaseMaterial3D.SHADING_MODE_UNSHADED
        or distant_material.shading_mode != BaseMaterial3D.SHADING_MODE_UNSHADED
    ):
        push_error("Terrain material regained a normal-lighting pass that exposes LOD seams")
        quit(66)
        return true

    var flat_rock := SurfaceVisual.terrain_color(
        800.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0
    )
    var steep_rock := SurfaceVisual.terrain_color(
        800.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.45
    )
    var slope_color_delta := (
        absf(flat_rock.r - steep_rock.r)
        + absf(flat_rock.g - steep_rock.g)
        + absf(flat_rock.b - steep_rock.b)
    )
    if slope_color_delta < 0.08:
        push_error("Steep terrain no longer receives a visible rock presentation cue")
        quit(58)
        return true

    var sun_facing := SurfaceVisual.relief_light(Vector3(-0.43, 0.76, -0.49))
    var lee_facing := SurfaceVisual.relief_light(Vector3(0.43, 0.76, 0.49))
    if sun_facing - lee_facing < 0.18:
        push_error("Seam-safe aspect hillshade no longer separates sun and lee slopes")
        quit(62)
        return true

    # The first focused simulation step refines the authoritative cover. Its
    # terrain revision must replace both resources under the player immediately.
    var sim := root.get_node("WorldViewer/Simulation") as WorldSimulationNode
    var revision_before := sim.get_terrain_revision()
    var surface_revision_before := sim.get_surface_revision()
    var mesh_before := mesh_instance.mesh
    var shape_before := collision.shape
    scene.call("_process", 0.25)
    if sim.get_terrain_revision() <= revision_before:
        push_error("Focused adaptive cover did not advance terrain revision")
        quit(8)
        return true
    if sim.get_surface_revision() <= surface_revision_before:
        push_error("Focused adaptive cover did not advance surface revision")
        quit(45)
        return true
    mesh = mesh_instance.mesh as ArrayMesh
    if mesh == mesh_before or collision.shape == shape_before:
        push_error("Terrain revision did not refresh mesh and collision resources")
        quit(9)
        return true
    if !_mesh_matches_collision(mesh, collision):
        push_error("Refreshed terrain mesh and collision heights disagree")
        quit(10)
        return true

    # Ecology/climate-only refreshes must not pay for another reconstructed
    # terrain/collision build. Force that path directly and ensure only the
    # visual mesh resource changes.
    var surface_mesh_before := mesh_instance.mesh
    var surface_shape_before := collision.shape
    var surface_dirty: Dictionary = scene.get("surface_dirty_chunks")
    surface_dirty[spawn_chunk] = true
    scene.set("surface_dirty_chunks", surface_dirty)
    scene.call("_create_chunk", spawn_chunk)
    if mesh_instance.mesh == surface_mesh_before:
        push_error("Surface-only refresh did not update near terrain colors")
        quit(73)
        return true
    if collision.shape != surface_shape_before:
        push_error("Surface-only refresh rebuilt walking collision terrain")
        quit(73)
        return true

    print(
        "WORLDSIM_TERRAIN_VIEW_OK winding=clockwise_from_+Y aabb_y=[%.2f, %.2f] sea_drop_262km=%.2f mountain_span_40km=%.2f mountain_prominence=%.2f view_distance_km=%.2f view_angle_deg=%.2f skyline_margin_deg=%.2f height_rise_m=%.2f distant_resolution=%d fov=%.1f spawn=[%.1f, %.1f] target=[%.1f, %.1f]"
        % [
            aabb.position.y,
            aabb.end.y,
            sea_drop_m,
            mountain_span,
            mountain_prominence,
            mountain_view_distance_m / 1000.0,
            rad_to_deg(best_elevation_angle),
            rad_to_deg(best_skyline_margin),
            height_rise_m,
            distant_resolution,
            camera.fov,
            spawn_east_m,
            spawn_north_m,
            mountain_target_east_m,
            mountain_target_north_m,
        ]
    )
    quit(0)
    return true

func _surface_packet_valid(surface: Dictionary, expected: int) -> bool:
    for key in [
        "grass_density_kg_m2",
        "shrub_density_kg_m2",
        "tree_density_kg_m2",
    ]:
        var values: PackedFloat32Array = surface.get(key, PackedFloat32Array())
        if values.size() != expected:
            return false
        for value in values:
            if not is_finite(value) or value < 0.0:
                return false

    for key in [
        "snow_cover_fraction",
        "flooded_fraction",
        "fire_active_fraction",
        "fire_burned_fraction",
    ]:
        var values: PackedFloat32Array = surface.get(key, PackedFloat32Array())
        if values.size() != expected:
            return false
        for value in values:
            if not is_finite(value) or value < 0.0 or value > 1.0:
                return false
    return true

func _face_normal(verts: PackedVector3Array, indices: PackedInt32Array, start: int) -> Vector3:
    var a := verts[indices[start]]
    var b := verts[indices[start + 1]]
    var c := verts[indices[start + 2]]
    return (b - a).cross(c - a)

func _mesh_matches_collision(mesh: ArrayMesh, collision: CollisionShape3D) -> bool:
    if collision == null or !(collision.shape is HeightMapShape3D):
        return false
    var verts: PackedVector3Array = mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX]
    var map_data: PackedFloat32Array = (collision.shape as HeightMapShape3D).map_data
    if verts.size() != map_data.size():
        return false
    for i in range(verts.size()):
        if absf(verts[i].y - float(map_data[i]) * collision.scale.y) > 0.01:
            return false
    return true

extends SceneTree

# Catches the inverted-winding failure: Godot 4.7 uses clockwise front faces,
# so a +Y right-hand winding was culled and the terrain collapsed to a noisy
# horizon line. https://docs.godotengine.org/en/4.7/classes/class_arraymesh.html

var scene: Node

func _initialize() -> void:
    var packed := load("res://main.tscn")
    scene = packed.instantiate()
    root.add_child(scene)

func _process(_delta: float) -> bool:
    if Engine.get_process_frames() < 12:
        return false

    var terrain := root.get_node_or_null("WorldViewer/Terrain")
    if terrain == null:
        push_error("Terrain node missing from main.tscn")
        quit(1)
        return true

    var mesh_instance := terrain.get_node_or_null("Chunk_0_0/Mesh") as MeshInstance3D
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
    var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
    if verts.size() < 3 or indices.size() < 6:
        push_error("Terrain mesh arrays are too small")
        quit(4)
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

    var collision := terrain.get_node_or_null("Chunk_0_0/Body/Collision") as CollisionShape3D
    if !_mesh_matches_collision(mesh, collision):
        push_error("Initial terrain mesh and collision heights disagree")
        quit(7)
        return true

    var distant_root := root.get_node_or_null("WorldViewer/DistantTerrain")
    var distant_terrain := root.get_node_or_null("WorldViewer/DistantTerrain/Lod8/Terrain") as MeshInstance3D
    var distant_ocean := root.get_node_or_null("WorldViewer/DistantTerrain/Lod8/Ocean") as MeshInstance3D
    var near_ocean := root.get_node_or_null("WorldViewer/DistantTerrain/Lod0/Ocean") as MeshInstance3D
    if distant_root == null or distant_terrain == null or distant_ocean == null or near_ocean == null:
        push_error("Spherical distant terrain/ocean nodes were not created")
        quit(11)
        return true
    if distant_terrain.mesh == null or distant_ocean.mesh == null or near_ocean.mesh == null:
        push_error("Spherical distant terrain/ocean meshes were not built")
        quit(12)
        return true

    var sea_arrays := (distant_ocean.mesh as ArrayMesh).surface_get_arrays(0)
    var sea_verts: PackedVector3Array = sea_arrays[Mesh.ARRAY_VERTEX]
    var distant_resolution := 33
    var center_index := 16 * distant_resolution + 16
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
    if camera.far < 300000.0:
        push_error("Camera far plane does not expose planetary terrain: %.1f" % camera.far)
        quit(15)
        return true
    if (
        world_environment.environment.fog_mode != Environment.FOG_MODE_DEPTH
        or world_environment.environment.fog_depth_end < 250000.0
    ):
        push_error("Environment does not use long-range depth fog")
        quit(16)
        return true

    # The first focused simulation step refines the authoritative cover. Its
    # terrain revision must replace both resources under the player immediately.
    var sim := root.get_node("WorldViewer/Simulation") as WorldSimulationNode
    var revision_before := sim.get_terrain_revision()
    var mesh_before := mesh_instance.mesh
    var shape_before := collision.shape
    scene.call("_process", 0.25)
    if sim.get_terrain_revision() <= revision_before:
        push_error("Focused adaptive cover did not advance terrain revision")
        quit(8)
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

    print(
        "WORLDSIM_TERRAIN_VIEW_OK winding=clockwise_from_+Y aabb_y=[%.2f, %.2f] sea_drop_262km=%.2f"
        % [aabb.position.y, aabb.end.y, sea_drop_m]
    )
    quit(0)
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

extends SceneTree

# Catches the inverted-winding failure: Godot 4.7 uses clockwise front faces,
# so a +Y right-hand winding was culled and the terrain collapsed to a noisy
# horizon line. https://docs.godotengine.org/en/4.7/classes/class_arraymesh.html

func _initialize() -> void:
    var packed := load("res://main.tscn")
    var scene: Node = packed.instantiate()
    root.add_child(scene)

func _process(_delta: float) -> bool:
    if Engine.get_process_frames() < 2:
        return false

    var terrain := root.get_node_or_null("WorldViewer/Terrain")
    if terrain == null:
        push_error("Terrain node missing from main.tscn")
        quit(1)
        return true

    var meshes := terrain.find_children("*", "MeshInstance3D", true, false)
    if meshes.is_empty():
        push_error("No terrain MeshInstance3D was created")
        quit(2)
        return true

    var mesh_instance: MeshInstance3D = meshes[0]
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

    print("WORLDSIM_TERRAIN_VIEW_OK winding=clockwise_from_+Y aabb_y=[%.2f, %.2f]" % [aabb.position.y, aabb.end.y])
    quit(0)
    return true

func _face_normal(verts: PackedVector3Array, indices: PackedInt32Array, start: int) -> Vector3:
    var a := verts[indices[start]]
    var b := verts[indices[start + 1]]
    var c := verts[indices[start + 2]]
    return (b - a).cross(c - a)

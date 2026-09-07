extends SceneTree

func _initialize() -> void:
    var packed := load("res://world_map.tscn")
    var scene: Node = packed.instantiate()
    root.add_child(scene)

func _process(_delta: float) -> bool:
    if Engine.get_process_frames() < 2:
        return false

    var map_view := root.get_node_or_null("WorldMapViewer/Margin/VBox/MapPanel/Map") as TextureRect
    if map_view == null:
        push_error("Global map TextureRect is missing")
        quit(1)
        return true
    if map_view.texture == null:
        push_error("Global map texture was not generated")
        quit(2)
        return true

    var image := map_view.texture.get_image()
    if image == null:
        push_error("Global map texture has no image")
        quit(3)
        return true
    if image.get_width() != 1024 or image.get_height() != 512:
        push_error("Global map texture size mismatch: %dx%d" % [image.get_width(), image.get_height()])
        quit(4)
        return true

    print("WORLDSIM_GLOBAL_MAP_VIEW_OK size=%dx%d" % [image.get_width(), image.get_height()])
    quit(0)
    return true

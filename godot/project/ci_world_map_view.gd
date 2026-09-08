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

    var elevation_image := map_view.texture.get_image()
    if elevation_image == null:
        push_error("Global map texture has no image")
        quit(3)
        return true
    if elevation_image.get_width() != 1024 or elevation_image.get_height() != 512:
        push_error("Global map texture size mismatch: %dx%d" % [
            elevation_image.get_width(), elevation_image.get_height()
        ])
        quit(4)
        return true

    var plates_button := root.get_node_or_null("WorldMapViewer/Margin/VBox/LayerBar/Plates") as Button
    var forcing_button := root.get_node_or_null("WorldMapViewer/Margin/VBox/LayerBar/Forcing") as Button
    var crust_button := root.get_node_or_null("WorldMapViewer/Margin/VBox/LayerBar/Crust") as Button
    var macro_button := root.get_node_or_null("WorldMapViewer/Margin/VBox/LayerBar/MacroRelief") as Button
    if (
        plates_button == null or
        forcing_button == null or
        crust_button == null or
        macro_button == null
    ):
        push_error("Global map tectonic layer controls are missing")
        quit(5)
        return true

    var elevation_data := elevation_image.get_data()
    plates_button.emit_signal("pressed")
    var plates_image := map_view.texture.get_image()
    if plates_image.get_data() == elevation_data or !_has_variation(plates_image):
        push_error("Plate layer did not render a distinct partition map")
        quit(6)
        return true
    if !_has_dark_plate_boundary(plates_image):
        push_error("Plate layer did not render explicit topology boundaries")
        quit(10)
        return true

    var plates_data := plates_image.get_data()
    forcing_button.emit_signal("pressed")
    var forcing_image := map_view.texture.get_image()
    if forcing_image.get_data() == plates_data or !_has_variation(forcing_image):
        push_error("Tectonic forcing layer did not render a distinct field")
        quit(7)
        return true

    var neutral: Color = scene.call("_forcing_color", 0.0, 0.0)
    var convergent: Color = scene.call("_forcing_color", 1.0, 0.0)
    var divergent: Color = scene.call("_forcing_color", 0.0, 1.0)
    if (
        neutral.r < 0.20 or
        convergent.r <= convergent.b or
        divergent.b <= divergent.r
    ):
        push_error("Tectonic response palette is not neutral/red/blue centered")
        quit(11)
        return true

    var forcing_data := forcing_image.get_data()
    crust_button.emit_signal("pressed")
    var crust_image := map_view.texture.get_image()
    if crust_image.get_data() == forcing_data or !_has_variation(crust_image):
        push_error("Crust affinity layer did not render a distinct field")
        quit(8)
        return true

    var crust_data := crust_image.get_data()
    macro_button.emit_signal("pressed")
    var macro_image := map_view.texture.get_image()
    if (
        macro_image.get_data() == crust_data or
        macro_image.get_data() == elevation_data or
        !_has_variation(macro_image)
    ):
        push_error("Macro relief layer did not render a distinct field")
        quit(9)
        return true

    print("WORLDSIM_GLOBAL_MAP_VIEW_OK size=%dx%d layers=elevation,plates,forcing,crust,macro" % [
        macro_image.get_width(), macro_image.get_height()
    ])
    quit(0)
    return true

func _has_dark_plate_boundary(image: Image) -> bool:
    for y in range(image.get_height()):
        for x in range(image.get_width()):
            var color := image.get_pixel(x, y)
            if color.r < 0.15 and color.g < 0.15 and color.b < 0.15:
                return true
    return false

func _has_variation(image: Image) -> bool:
    var first := image.get_pixel(0, 0)
    for y in range(0, image.get_height(), 16):
        for x in range(0, image.get_width(), 16):
            if image.get_pixel(x, y) != first:
                return true
    return false

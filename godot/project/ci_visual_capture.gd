extends SceneTree

const CAPTURE_WIDTH := 1600
const CAPTURE_HEIGHT := 900
const NEAR_BUILD_STEPS := 64
const DISTANT_BUILD_STEPS := 16
const BENCHMARK_FRAMES := 60

var scene: Node
var capture_start_ms := 0

func _initialize() -> void:
    capture_start_ms = Time.get_ticks_msec()
    if DisplayServer.get_name() == "headless":
        push_error("Visual capture requires a rendered display driver, not --headless")
        quit(1)
        return

    var packed := load("res://main.tscn") as PackedScene
    if packed == null:
        push_error("Visual capture could not load main.tscn")
        quit(2)
        return

    scene = packed.instantiate()
    root.add_child(scene)

    # Freeze automatic gameplay/simulation updates after _ready(). The capture
    # script advances only the existing presentation build queues with zero delta.
    scene.process_mode = Node.PROCESS_MODE_DISABLED
    call_deferred("_prepare_and_capture")

func _prepare_and_capture() -> void:
    var distant := root.get_node_or_null("WorldViewer/DistantTerrain")
    if distant == null:
        push_error("Visual capture could not find distant terrain")
        quit(3)
        return

    for _i in range(NEAR_BUILD_STEPS):
        scene.call("_process", 0.0)
    for _i in range(DISTANT_BUILD_STEPS):
        distant.call("_process", 0.0)

    var pending_chunks: Array = scene.get("pending_chunks")
    var pending_levels: Array = distant.get("pending_levels")
    if !pending_chunks.is_empty() or !pending_levels.is_empty():
        push_error(
            "Visual capture presentation queues did not drain: near=%d distant=%d"
            % [pending_chunks.size(), pending_levels.size()]
        )
        quit(4)
        return

    await RenderingServer.frame_post_draw

    var image := root.get_texture().get_image()
    if image == null or image.is_empty():
        push_error("Visual capture produced an empty viewport image")
        quit(5)
        return
    if image.get_width() != CAPTURE_WIDTH or image.get_height() != CAPTURE_HEIGHT:
        push_error(
            "Visual capture resolution mismatch: got %dx%d expected %dx%d"
            % [
                image.get_width(),
                image.get_height(),
                CAPTURE_WIDTH,
                CAPTURE_HEIGHT,
            ]
        )
        quit(6)
        return

    var luminance_min := 1.0
    var luminance_max := 0.0
    for y in range(0, image.get_height(), 90):
        for x in range(0, image.get_width(), 100):
            var color := image.get_pixel(x, y)
            var luminance := (
                0.2126 * color.r
                + 0.7152 * color.g
                + 0.0722 * color.b
            )
            luminance_min = minf(luminance_min, luminance)
            luminance_max = maxf(luminance_max, luminance)
    if luminance_max - luminance_min < 0.05:
        push_error(
            "Visual capture frame is trivially uniform: luminance range=%.4f"
            % (luminance_max - luminance_min)
        )
        quit(7)
        return

    var output_dir := OS.get_environment("WORLDSIM_VISUAL_CAPTURE_DIR")
    if output_dir.is_empty():
        push_error("WORLDSIM_VISUAL_CAPTURE_DIR is not set")
        quit(8)
        return

    var benchmark_start_ms := Time.get_ticks_msec()
    for _i in range(BENCHMARK_FRAMES):
        await RenderingServer.frame_post_draw
    var benchmark_elapsed_ms := maxi(
        Time.get_ticks_msec() - benchmark_start_ms,
        1
    )
    var rendered_fps := (
        1000.0 * float(BENCHMARK_FRAMES) / float(benchmark_elapsed_ms)
    )

    var spawn_chunk: Vector2i = scene.get("current_chunk")
    var dirty_chunks: Dictionary = scene.get("dirty_chunks")
    dirty_chunks[spawn_chunk] = true
    scene.set("dirty_chunks", dirty_chunks)
    var terrain_rebuild_start_ms := Time.get_ticks_msec()
    scene.call("_create_chunk", spawn_chunk)
    var terrain_rebuild_ms := (
        Time.get_ticks_msec() - terrain_rebuild_start_ms
    )

    var surface_dirty_chunks: Dictionary = scene.get("surface_dirty_chunks")
    surface_dirty_chunks[spawn_chunk] = true
    scene.set("surface_dirty_chunks", surface_dirty_chunks)
    var surface_refresh_start_ms := Time.get_ticks_msec()
    scene.call("_create_chunk", spawn_chunk)
    var surface_refresh_ms := (
        Time.get_ticks_msec() - surface_refresh_start_ms
    )

    var output_path := output_dir.path_join("walk_spawn.png")
    var save_error := image.save_png(output_path)
    if save_error != OK:
        push_error(
            "Visual capture could not save PNG: error=%d path=%s"
            % [save_error, output_path]
        )
        quit(9)
        return

    print(
        "WORLDSIM_GODOT_VISUAL_OK display=%s size=%dx%d luminance_range=%.4f capture_ms=%d rendered_fps=%.2f terrain_rebuild_ms=%d surface_refresh_ms=%d path=%s"
        % [
            DisplayServer.get_name(),
            image.get_width(),
            image.get_height(),
            luminance_max - luminance_min,
            Time.get_ticks_msec() - capture_start_ms,
            rendered_fps,
            terrain_rebuild_ms,
            surface_refresh_ms,
            output_path,
        ]
    )
    quit(0)

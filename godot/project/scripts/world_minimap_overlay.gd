extends Control

var marker_uv := Vector2(0.5, 0.5)
var heading_uv_delta := Vector2(0.0, -0.01)

func set_marker(next_marker_uv: Vector2, next_heading_uv_delta: Vector2) -> void:
    marker_uv = Vector2(fposmod(next_marker_uv.x, 1.0), clampf(next_marker_uv.y, 0.0, 1.0))
    heading_uv_delta = next_heading_uv_delta
    queue_redraw()

func _draw() -> void:
    if size.x <= 0.0 or size.y <= 0.0:
        return

    var grid_color := Color(0.78, 0.88, 1.0, 0.12)
    for fraction in [0.25, 0.5, 0.75]:
        var x: float = size.x * float(fraction)
        draw_line(Vector2(x, 0.0), Vector2(x, size.y), grid_color, 1.0, true)
    for fraction in [0.25, 0.75]:
        var y: float = size.y * float(fraction)
        draw_line(Vector2(0.0, y), Vector2(size.x, y), grid_color, 1.0, true)
    draw_line(
        Vector2(0.0, size.y * 0.5),
        Vector2(size.x, size.y * 0.5),
        Color(0.92, 0.78, 0.42, 0.18),
        1.0,
        true
    )

    var center := Vector2(marker_uv.x * size.x, marker_uv.y * size.y)
    var heading_pixels := Vector2(
        heading_uv_delta.x * size.x,
        heading_uv_delta.y * size.y
    )
    var heading := heading_pixels.normalized() if !heading_pixels.is_zero_approx() else Vector2.UP
    for wrap_offset in [-size.x, 0.0, size.x]:
        _draw_marker(center + Vector2(wrap_offset, 0.0), heading)

func _draw_marker(center: Vector2, heading: Vector2) -> void:
    var side := Vector2(-heading.y, heading.x)
    var arrow := PackedVector2Array([
        center + heading * 16.0,
        center - heading * 7.0 + side * 7.0,
        center - heading * 4.0,
        center - heading * 7.0 - side * 7.0,
    ])
    var shadow := PackedVector2Array()
    for point in arrow:
        shadow.push_back(point + Vector2(0.0, 2.0))
    draw_colored_polygon(shadow, Color(0.0, 0.0, 0.0, 0.72))
    draw_colored_polygon(arrow, Color(0.28, 0.91, 1.0))

    var outline := PackedVector2Array(arrow)
    outline.push_back(arrow[0])
    draw_polyline(outline, Color(0.94, 1.0, 1.0), 1.5, true)
    draw_circle(center, 5.0, Color(0.025, 0.055, 0.09, 0.95))
    draw_circle(center, 2.6, Color(1.0, 0.84, 0.30))

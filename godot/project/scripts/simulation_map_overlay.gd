extends Control

var _marker_uv := Vector2(0.5, 0.5)
var _focused := false

func set_marker(uv: Vector2, focused: bool) -> void:
    _marker_uv = Vector2(clampf(uv.x, 0.0, 1.0), clampf(uv.y, 0.0, 1.0))
    _focused = focused
    queue_redraw()

func _draw() -> void:
    if size.x <= 1.0 or size.y <= 1.0:
        return
    var point := Vector2(_marker_uv.x * size.x, _marker_uv.y * size.y)
    var color := Color(1.0, 0.76, 0.22) if !_focused else Color(0.25, 0.92, 1.0)
    draw_circle(point, 8.0, Color(0.0, 0.0, 0.0, 0.72), true)
    draw_arc(point, 9.0, 0.0, TAU, 28, color, 2.0, true)
    draw_line(point - Vector2(14.0, 0.0), point - Vector2(5.0, 0.0), color, 2.0)
    draw_line(point + Vector2(5.0, 0.0), point + Vector2(14.0, 0.0), color, 2.0)
    draw_line(point - Vector2(0.0, 14.0), point - Vector2(0.0, 5.0), color, 2.0)
    draw_line(point + Vector2(0.0, 5.0), point + Vector2(0.0, 14.0), color, 2.0)

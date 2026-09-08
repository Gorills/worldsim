extends Control

var _samples: Array[Vector2] = []

func set_samples(samples: Array[Vector2]) -> void:
    _samples = samples.duplicate()
    queue_redraw()

func get_sample_count() -> int:
    return _samples.size()

func _draw() -> void:
    var bounds := Rect2(Vector2(8.0, 8.0), size - Vector2(16.0, 16.0))
    if bounds.size.x <= 1.0 or bounds.size.y <= 1.0:
        return

    draw_rect(bounds, Color(0.025, 0.045, 0.075, 0.95), true)
    for grid_index in range(1, 4):
        var grid_y := bounds.position.y + bounds.size.y * float(grid_index) / 4.0
        draw_line(
            Vector2(bounds.position.x, grid_y),
            Vector2(bounds.end.x, grid_y),
            Color(0.25, 0.38, 0.52, 0.24),
            1.0
        )

    if _samples.size() < 2:
        if _samples.size() == 1:
            draw_circle(bounds.get_center(), 3.0, Color(0.35, 0.82, 1.0))
        return

    var min_x := _samples[0].x
    var max_x := min_x
    var min_y := _samples[0].y
    var max_y := min_y
    for sample in _samples:
        min_x = minf(min_x, sample.x)
        max_x = maxf(max_x, sample.x)
        min_y = minf(min_y, sample.y)
        max_y = maxf(max_y, sample.y)

    var x_span := maxf(max_x - min_x, 1.0)
    var y_span := max_y - min_y
    if is_zero_approx(y_span):
        y_span = maxf(absf(max_y) * 0.1, 1.0)
        min_y -= y_span * 0.5

    var points := PackedVector2Array()
    for sample in _samples:
        var normalized_x := (sample.x - min_x) / x_span
        var normalized_y := (sample.y - min_y) / y_span
        points.push_back(Vector2(
            bounds.position.x + normalized_x * bounds.size.x,
            bounds.end.y - normalized_y * bounds.size.y
        ))

    draw_polyline(points, Color(0.25, 0.78, 1.0), 2.0, true)
    draw_circle(points[points.size() - 1], 3.0, Color(1.0, 0.78, 0.28))


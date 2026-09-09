extends RefCounted

const GRASS_SATURATION_KG_M2 := 1.8
const SHRUB_SATURATION_KG_M2 := 3.5
const TREE_SATURATION_KG_M2 := 9.0
const RELIEF_LIGHT_DIRECTION := Vector3(-0.43, 0.76, -0.49)

static func cover_from_density(density_kg_m2: float, saturation_kg_m2: float) -> float:
    if saturation_kg_m2 <= 0.0:
        return 0.0
    return clampf(density_kg_m2 / saturation_kg_m2, 0.0, 1.0)

static func relief_light(normal: Vector3) -> float:
    var n := normal.normalized()
    var direct := maxf(n.dot(RELIEF_LIGHT_DIRECTION), 0.0)
    var sky_fill := clampf(n.y, 0.0, 1.0)
    return clampf(0.58 + 0.42 * direct + 0.10 * sky_fill, 0.60, 1.10)

static func _lattice_hash(x: int, z: int) -> float:
    var value := sin(float(x) * 12.9898 + float(z) * 78.233) * 43758.5453123
    return value - floor(value)

static func _value_noise(east_m: float, north_m: float, scale_m: float) -> float:
    var gx := east_m / scale_m
    var gz := north_m / scale_m
    var x0 := floori(gx)
    var z0 := floori(gz)
    var tx := gx - float(x0)
    var tz := gz - float(z0)
    var sx := tx * tx * (3.0 - 2.0 * tx)
    var sz := tz * tz * (3.0 - 2.0 * tz)
    var a := lerpf(
        _lattice_hash(x0, z0),
        _lattice_hash(x0 + 1, z0),
        sx
    )
    var b := lerpf(
        _lattice_hash(x0, z0 + 1),
        _lattice_hash(x0 + 1, z0 + 1),
        sx
    )
    return lerpf(a, b, sz) * 2.0 - 1.0

static func terrain_detail(east_m: float, north_m: float) -> float:
    # Pure world-space presentation noise: the same coordinate receives the
    # same tint in near chunks and every distant LOD, so it cannot create a
    # clipmap boundary by itself.
    var broad := _value_noise(east_m, north_m, 1400.0)
    var medium := _value_noise(east_m, north_m, 420.0)
    return clampf(0.64 * broad + 0.36 * medium, -1.0, 1.0)

static func terrain_color(
    height_m: float,
    grass_density_kg_m2: float,
    shrub_density_kg_m2: float,
    tree_density_kg_m2: float,
    snow_cover_fraction: float,
    flooded_fraction: float,
    fire_active_fraction: float,
    fire_burned_fraction: float,
    slope: float = 0.0,
    relief_light_factor: float = 1.0,
    detail_variation: float = 0.0
) -> Color:
    if height_m < 0.0:
        var depth_t := clampf(-height_m / 5000.0, 0.0, 1.0)
        return Color(0.25, 0.27, 0.28).lerp(
            Color(0.12, 0.14, 0.16),
            depth_t
        )

    var elevation_t := clampf(height_m / 4500.0, 0.0, 1.0)
    var color := Color(0.30, 0.255, 0.18).lerp(
        Color(0.44, 0.42, 0.39),
        elevation_t
    )

    var grass := cover_from_density(
        grass_density_kg_m2,
        GRASS_SATURATION_KG_M2
    )
    var shrub := cover_from_density(
        shrub_density_kg_m2,
        SHRUB_SATURATION_KG_M2
    )
    var tree := cover_from_density(
        tree_density_kg_m2,
        TREE_SATURATION_KG_M2
    )
    var vegetation_strength := clampf(
        0.55 * grass + 0.72 * shrub + 0.88 * tree,
        0.0,
        0.70
    )
    if vegetation_strength > 0.0:
        var weight_sum := grass + shrub + tree
        var vegetation_color := (
            Color(0.20, 0.36, 0.11) * grass
            + Color(0.14, 0.28, 0.085) * shrub
            + Color(0.07, 0.20, 0.06) * tree
        ) / maxf(weight_sum, 0.0001)
        color = color.lerp(vegetation_color, vegetation_strength)

    # Coarse ecology fields do not encode local cliff exposure. Use the rendered
    # mesh slope only as a presentation cue so steep/high terrain reads as rock
    # without mutating authoritative surface state.
    var steepness := clampf((slope - 0.06) / 0.48, 0.0, 1.0)
    var highland_rock := clampf((height_m - 1800.0) / 2600.0, 0.0, 1.0)
    var rock_strength := clampf(
        maxf(0.82 * steepness, 0.65 * highland_rock),
        0.0,
        0.85
    )
    if rock_strength > 0.0:
        var rock_color := Color(0.20, 0.19, 0.18).lerp(
            Color(0.44, 0.43, 0.42),
            elevation_t
        )
        color = color.lerp(rock_color, rock_strength)

    var detail := clampf(detail_variation, -1.0, 1.0)
    var detail_strength := 0.075 + 0.045 * steepness
    var detail_factor := 1.0 + detail_strength * detail
    color = Color(
        color.r * detail_factor,
        color.g * detail_factor,
        color.b * (1.0 + 0.75 * detail_strength * detail),
        color.a
    )

    var burned := clampf(fire_burned_fraction, 0.0, 1.0)
    if burned > 0.0:
        color = color.lerp(Color(0.13, 0.10, 0.08), 0.78 * burned)

    var active_fire := clampf(fire_active_fraction, 0.0, 1.0)
    if active_fire > 0.0:
        color = color.lerp(Color(0.48, 0.19, 0.055), 0.55 * active_fire)

    var flooded := clampf(flooded_fraction, 0.0, 1.0)
    if flooded > 0.0:
        color = color.lerp(Color(0.10, 0.20, 0.22), 0.65 * flooded)

    var snow := clampf(snow_cover_fraction, 0.0, 1.0)
    if snow > 0.0:
        color = color.lerp(Color(0.91, 0.94, 0.96), 0.92 * snow)

    var light_factor := clampf(relief_light_factor, 0.60, 1.10)
    return Color(
        color.r * light_factor,
        color.g * light_factor,
        color.b * light_factor,
        color.a
    )

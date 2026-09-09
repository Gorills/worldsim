extends RefCounted

const GRASS_SATURATION_KG_M2 := 1.8
const SHRUB_SATURATION_KG_M2 := 3.5
const TREE_SATURATION_KG_M2 := 9.0

static func cover_from_density(density_kg_m2: float, saturation_kg_m2: float) -> float:
    if saturation_kg_m2 <= 0.0:
        return 0.0
    return clampf(density_kg_m2 / saturation_kg_m2, 0.0, 1.0)

static func terrain_color(
    height_m: float,
    grass_density_kg_m2: float,
    shrub_density_kg_m2: float,
    tree_density_kg_m2: float,
    snow_cover_fraction: float,
    flooded_fraction: float,
    fire_active_fraction: float,
    fire_burned_fraction: float,
    slope: float = 0.0
) -> Color:
    if height_m < 0.0:
        var depth_t := clampf(-height_m / 5000.0, 0.0, 1.0)
        return Color(0.25, 0.27, 0.28).lerp(
            Color(0.12, 0.14, 0.16),
            depth_t
        )

    var elevation_t := clampf(height_m / 4500.0, 0.0, 1.0)
    var color := Color(0.31, 0.27, 0.21).lerp(
        Color(0.52, 0.50, 0.47),
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
        0.85
    )
    if vegetation_strength > 0.0:
        var weight_sum := grass + shrub + tree
        var vegetation_color := (
            Color(0.22, 0.42, 0.15) * grass
            + Color(0.16, 0.31, 0.11) * shrub
            + Color(0.08, 0.22, 0.07) * tree
        ) / maxf(weight_sum, 0.0001)
        color = color.lerp(vegetation_color, vegetation_strength)

    # Coarse ecology fields do not encode local cliff exposure. Use the rendered
    # mesh slope only as a presentation cue so steep/high terrain reads as rock
    # without mutating authoritative surface state.
    var steepness := clampf((slope - 0.06) / 0.48, 0.0, 1.0)
    var highland_rock := clampf((height_m - 1800.0) / 2600.0, 0.0, 1.0)
    var rock_strength := clampf(
        maxf(0.75 * steepness, 0.65 * highland_rock),
        0.0,
        0.82
    )
    if rock_strength > 0.0:
        var rock_color := Color(0.20, 0.19, 0.18).lerp(
            Color(0.52, 0.51, 0.50),
            elevation_t
        )
        color = color.lerp(rock_color, rock_strength)

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

    return color

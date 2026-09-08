#include "worldsim/geology.hpp"

#include <algorithm>
#include <cmath>

namespace worldsim {
namespace {

double smoothstep01(double x) {
    const double t=std::clamp(x,0.0,1.0);
    return t*t*(3.0-2.0*t);
}

double lerp(double a, double b, double t) {
    return a+(b-a)*t;
}

} // namespace

GeologyState GeologyModel::initial_state(Vec3d direction, double cell_area_m2) const {
    const TectonicSample tectonic=tectonics_.sample_direction(direction);
    const double affinity=std::clamp(tectonic.continental_affinity,0.0,1.0);

    const double base_crust=lerp(7'000.0,35'000.0,affinity);
    const double thickening=(4'000.0+7'000.0*affinity)*tectonic.uplift_forcing;
    const double rift_thinning=3'500.0*affinity*tectonic.divergence_forcing;
    const double crust_thickness=std::clamp(
        base_crust+thickening-rift_thinning,
        3'000.0,
        60'000.0
    );
    const double crust_density=lerp(2'950.0,2'800.0,affinity);

    const std::uint64_t pair_key=
        (static_cast<std::uint64_t>(tectonic.plate_id)<<32U) |
        static_cast<std::uint64_t>(tectonic.neighbor_plate_id);
    const double age_noise=deterministic_unit(
        seed_,
        fnv1a64("geology.lithosphere.age"),
        0,
        pair_key
    );
    const double ocean_age=(4.0+156.0*(0.25+0.75*age_noise))*
        (1.0-0.94*tectonic.divergence_forcing);
    const double continental_age=500.0+2'500.0*age_noise;
    const double age=lerp(
        std::max(0.5,ocean_age),
        continental_age,
        smoothstep01((affinity-0.35)/0.35)
    );

    const TerrainSample preview=terrain_.sample_direction(direction);
    const double lowland=std::exp(-std::pow(preview.elevation_m/2'500.0,2.0));
    const double sediment_thickness=
        80.0+520.0*lowland+260.0*(1.0-affinity);
    const double sediment_mass=sediment_thickness*cell_area_m2*kSedimentDensityKgM3;

    const double regolith=std::clamp(
        affinity*(1.0+7.0*lowland),
        0.0,
        20.0
    );

    return {
        crust_thickness,
        crust_density,
        age,
        sediment_mass,
        regolith
    };
}

void GeologyModel::advance_tectonics(
    GeologyState& state,
    Vec3d direction,
    double dt_years
) const {
    if (!(dt_years>0.0)) return;
    const TectonicSample tectonic=tectonics_.sample_direction(direction);
    const double affinity=std::clamp(tectonic.continental_affinity,0.0,1.0);
    const double dt_ma=dt_years/1.0e6;

    state.lithosphere_age_ma=std::clamp(
        state.lithosphere_age_ma+dt_ma,
        0.0,
        4'500.0
    );

    if (tectonic.divergence_forcing>0.0) {
        if (affinity<0.45) {
            // Oceanic spreading creates young crust and relaxes its thickness
            // toward a thin basaltic reference column.
            const double renewal=1.0-std::exp(
                -tectonic.divergence_forcing*dt_ma/2.0
            );
            state.lithosphere_age_ma*=1.0-renewal;
            state.crust_thickness_m=lerp(
                state.crust_thickness_m,
                7'000.0,
                renewal
            );
        } else {
            // Continental rifting thins buoyant crust before breakup.
            state.crust_thickness_m-=650.0*
                tectonic.divergence_forcing*dt_ma;
        }
    }

    if (tectonic.uplift_forcing>0.0) {
        if (affinity>=0.45) {
            // Continental convergence/collision stores shortening as crustal
            // thickening, which subsequently drives isostatic uplift.
            state.crust_thickness_m+=750.0*
                tectonic.uplift_forcing*dt_ma;
        } else {
            // Oceanic convergence consumes crust into the mantle reservoir.
            state.crust_thickness_m-=420.0*
                tectonic.uplift_forcing*dt_ma;
        }
    }

    state.crust_thickness_m=std::clamp(
        state.crust_thickness_m,
        3'000.0,
        70'000.0
    );

    const double target_density=lerp(2'950.0,2'800.0,affinity);
    const double density_relax=1.0-std::exp(-0.04*dt_ma);
    state.crust_density_kg_m3=lerp(
        state.crust_density_kg_m3,
        target_density,
        density_relax
    );

    // Weathering continuously rebuilds a finite mobile regolith mantle.
    state.regolith_thickness_m=std::clamp(
        state.regolith_thickness_m+0.45*affinity*dt_ma,
        0.0,
        30.0
    );
}

double GeologyModel::ocean_floor_elevation_m(double age_ma) const {
    const double age=std::clamp(age_ma,0.0,200.0);
    if (age<=70.0)
        return -(2'500.0+350.0*std::sqrt(age));
    return -(6'400.0-3'200.0*std::exp(-age/62.8));
}

double GeologyModel::surface_elevation_m(
    const GeologyState& state,
    Vec3d direction,
    double cell_area_m2
) const {
    const TectonicSample tectonic=tectonics_.sample_direction(direction);
    const double affinity=std::clamp(tectonic.continental_affinity,0.0,1.0);
    const double continental_weight=smoothstep01((affinity-0.20)/0.60);

    const double isostatic_continent=
        -4'550.0+
        state.crust_thickness_m*
        (kMantleDensityKgM3-state.crust_density_kg_m3)/
        kMantleDensityKgM3;
    const double oceanic=ocean_floor_elevation_m(state.lithosphere_age_ma);
    double elevation=lerp(oceanic,isostatic_continent,continental_weight);

    const double area=std::max(1.0,cell_area_m2);
    const double sediment_thickness=
        state.sediment_mass_kg/(area*kSedimentDensityKgM3);
    elevation-=0.65*sediment_thickness*
        kSedimentDensityKgM3/kMantleDensityKgM3;

    // Convergent oceanic margins form trenches while continental overriding
    // crust and continent-continent collisions remain positive. Continental
    // divergence retains the rift depression not captured by thermal age.
    elevation+=900.0*tectonic.uplift_forcing*affinity;
    elevation-=1'600.0*tectonic.uplift_forcing*(1.0-affinity);
    elevation-=800.0*tectonic.divergence_forcing*affinity;

    // Preserve bounded sphere-native meso/local roughness while replacing the
    // old tectonic macro height with the stateful geological equilibrium.
    const TerrainSample preview=terrain_.sample_direction(direction);
    elevation+=0.55*(preview.elevation_m-tectonic.macro_elevation_m);

    return std::clamp(elevation,-11'000.0,9'000.0);
}

double GeologyModel::erosion_rate_m_per_year(
    double downhill_slope,
    double runoff_m_per_day,
    double regolith_thickness_m
) const {
    if (!(downhill_slope>0.0)) return 0.0;
    const double runoff_mm_day=std::max(0.0,runoff_m_per_day)*1'000.0;
    const double water_factor=std::sqrt(0.05+runoff_mm_day/3.0);
    const double slope_factor=std::pow(
        std::max(0.0,downhill_slope)/0.05,
        1.1
    );
    const double cover_factor=0.65+0.35*std::clamp(
        regolith_thickness_m/2.0,
        0.0,
        1.0
    );
    return std::clamp(
        2.0e-4*water_factor*slope_factor*cover_factor,
        0.0,
        5.0e-3
    );
}

ErosionBudget GeologyModel::erode(
    GeologyState& state,
    double cell_area_m2,
    double erosion_depth_m
) const {
    ErosionBudget budget;
    if (!(erosion_depth_m>0.0) || !(cell_area_m2>0.0)) return budget;

    const double sediment_depth=
        state.sediment_mass_kg/(cell_area_m2*kSedimentDensityKgM3);
    const double sediment_removed_depth=std::min(
        sediment_depth,
        erosion_depth_m
    );
    budget.sediment_removed_kg=
        sediment_removed_depth*cell_area_m2*kSedimentDensityKgM3;
    state.sediment_mass_kg=std::max(
        0.0,
        state.sediment_mass_kg-budget.sediment_removed_kg
    );

    const double remaining_depth=erosion_depth_m-sediment_removed_depth;
    const double removable_crust=std::max(
        0.0,
        state.crust_thickness_m-3'000.0
    );
    budget.crust_thickness_removed_m=std::min(
        remaining_depth,
        removable_crust
    );
    budget.bedrock_removed_kg=
        budget.crust_thickness_removed_m*
        cell_area_m2*
        state.crust_density_kg_m3;
    state.crust_thickness_m-=budget.crust_thickness_removed_m;
    state.regolith_thickness_m=std::max(
        0.0,
        state.regolith_thickness_m-erosion_depth_m
    );
    return budget;
}

void GeologyModel::deposit(GeologyState& state, double sediment_mass_kg) const {
    if (sediment_mass_kg>0.0)
        state.sediment_mass_kg+=sediment_mass_kg;
}

} // namespace worldsim

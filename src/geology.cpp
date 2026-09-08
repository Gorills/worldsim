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

constexpr double kMaximumRegolithProductionMPerYear=1.0e-4;
constexpr double kRegolithProductionDecayDepthM=1.0;

double regolith_production_rate_m_per_year(
    double regolith_thickness_m,
    double continental_fraction
) {
    const double continental=std::clamp(
        continental_fraction,
        0.0,
        1.0
    );
    if (!(continental>0.0)) return 0.0;

    const double depth=std::max(0.0,regolith_thickness_m);
    return kMaximumRegolithProductionMPerYear*
        continental*
        std::exp(-depth/kRegolithProductionDecayDepthM);
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
    // A present-day-like ocean floor cannot be initialized from a uniform
    // 0..160 Ma age distribution: continuous creation and subduction skew the
    // surviving area toward younger lithosphere. Preserve the empirical upper
    // range while biasing deterministic initial ages young.
    const double ocean_age=(
        2.0+148.0*std::pow(age_noise,1.6)
    )*(1.0-0.95*tectonic.divergence_forcing);
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
    const double sediment_mass=sediment_mass_for_thickness_kg(
        sediment_thickness,
        cell_area_m2
    );

    const double regolith=std::clamp(
        affinity*(1.0+7.0*lowland),
        0.0,
        20.0
    );

    return {
        crust_thickness,
        crust_density,
        affinity,
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
    const double dt_ma=dt_years/1.0e6;
    state.continental_fraction=std::clamp(
        state.continental_fraction,
        0.0,
        1.0
    );

    state.lithosphere_age_ma=std::clamp(
        state.lithosphere_age_ma+dt_ma,
        0.0,
        4'500.0
    );

    if (tectonic.divergence_forcing>0.0) {
        // Long-lived continental extension can complete breakup. The evolving
        // fraction, not the immutable seed affinity, decides when a column
        // crosses into oceanic spreading.
        state.continental_fraction=std::clamp(
            state.continental_fraction-
                0.05*tectonic.divergence_forcing*dt_ma,
            0.0,
            1.0
        );
        const double oceanic_share=smoothstep01(
            (0.55-state.continental_fraction)/0.25
        );
        const double continental_share=1.0-oceanic_share;
        state.crust_thickness_m-=650.0*
            tectonic.divergence_forcing*
            continental_share*
            dt_ma;

        // Once breakup has progressed far enough, spreading renews young thin
        // basaltic crust instead of thinning continental crust indefinitely.
        const double renewal=1.0-std::exp(
            -tectonic.divergence_forcing*
            oceanic_share*
            dt_ma/2.0
        );
        state.lithosphere_age_ma*=1.0-renewal;
        state.crust_thickness_m=lerp(
            state.crust_thickness_m,
            7'000.0,
            renewal
        );
    }

    if (tectonic.uplift_forcing>0.0 || tectonic.convergence>0.0) {
        const BoundaryFeatureSample features=boundary_features(
            state,
            direction
        );
        // Continent-continent convergence stores shortening on both sides.
        state.crust_thickness_m+=900.0*
            features.collision_forcing*
            dt_ma;
        // Subduction is deliberately asymmetric: only the selected
        // subducting side consumes crust, while the overriding side receives
        // modest magmatic crustal addition beneath the volcanic arc.
        state.crust_thickness_m-=520.0*
            features.trench_forcing*
            dt_ma;
        state.crust_thickness_m+=180.0*
            features.volcanic_arc_forcing*
            dt_ma;
    }

    state.crust_thickness_m=std::clamp(
        state.crust_thickness_m,
        3'000.0,
        70'000.0
    );

    const double target_density=lerp(
        2'950.0,
        2'800.0,
        state.continental_fraction
    );
    const double density_relax=1.0-std::exp(-0.04*dt_ma);
    state.crust_density_kg_m3=lerp(
        state.crust_density_kg_m3,
        target_density,
        density_relax
    );

    // Bedrock-to-mobile-mantle production is fastest where bedrock is
    // exposed and decreases exponentially beneath an existing regolith cover.
    // Integrate dH/dt = W0*C*exp(-H/H*) analytically so long geology steps do
    // not overproduce regolith relative to many shorter steps.
    state.regolith_thickness_m=std::clamp(
        state.regolith_thickness_m,
        0.0,
        30.0
    );
    const double production_rate=regolith_production_rate_m_per_year(
        state.regolith_thickness_m,
        state.continental_fraction
    );
    if (production_rate>0.0) {
        state.regolith_thickness_m=std::clamp(
            state.regolith_thickness_m+
                kRegolithProductionDecayDepthM*std::log1p(
                    production_rate*dt_years/
                    kRegolithProductionDecayDepthM
                ),
            0.0,
            30.0
        );
    }
}

double GeologyModel::ocean_floor_elevation_m(double age_ma) const {
    const double age=std::clamp(age_ma,0.0,200.0);
    if (age<=70.0)
        return -(2'500.0+350.0*std::sqrt(age));
    return -(6'400.0-3'200.0*std::exp(-age/62.8));
}

double GeologyModel::sediment_mass_for_thickness_kg(
    double sediment_thickness_m,
    double cell_area_m2
) const {
    if (!(sediment_thickness_m>0.0) || !(cell_area_m2>0.0))
        return 0.0;

    const double thickness=sediment_thickness_m;
    const double compacted_pore_depth=
        kSedimentSurfacePorosity*
        (-std::expm1(-kSedimentCompactionPerM*thickness))/
        kSedimentCompactionPerM;
    const double solid_depth=std::max(
        0.0,
        thickness-compacted_pore_depth
    );
    return solid_depth*cell_area_m2*kSedimentDensityKgM3;
}

double GeologyModel::sediment_column_thickness_m(
    double sediment_mass_kg,
    double cell_area_m2
) const {
    if (!(sediment_mass_kg>0.0) || !(cell_area_m2>0.0))
        return 0.0;

    const double solid_depth=
        sediment_mass_kg/(cell_area_m2*kSedimentDensityKgM3);
    double lo=solid_depth;
    double hi=solid_depth/(1.0-kSedimentSurfacePorosity);
    double thickness=0.5*(lo+hi);

    // Invert the integrated Athy porosity profile. The residual is strictly
    // monotone because 1-phi(z) stays positive, so the bracketed Newton step
    // is deterministic and cannot diverge for valid positive mass/area.
    for (int i=0;i<10;++i) {
        const double exp_term=std::exp(
            -kSedimentCompactionPerM*thickness
        );
        const double pore_depth=
            kSedimentSurfacePorosity*(1.0-exp_term)/
            kSedimentCompactionPerM;
        const double residual=
            thickness-pore_depth-solid_depth;
        if (
            std::abs(residual)<=
            1.0e-12*std::max(1.0,solid_depth)
        ) {
            return thickness;
        }
        if (residual>0.0) hi=thickness;
        else lo=thickness;

        const double derivative=
            1.0-kSedimentSurfacePorosity*exp_term;
        const double newton=thickness-residual/derivative;
        thickness=(newton>lo && newton<hi)
            ? newton
            : 0.5*(lo+hi);
    }
    return thickness;
}

BoundaryFeatureSample GeologyModel::boundary_features(
    const GeologyState& state,
    Vec3d direction
) const {
    const TectonicSample tectonic=tectonics_.sample_direction(direction);
    const double continental_fraction=std::clamp(
        state.continental_fraction,
        0.0,
        1.0
    );
    const double owner_continental_score=smoothstep01(
        (continental_fraction-0.60)/0.30
    );

    // Classify the convergent segment from crust on both sides of the actual
    // nearest plate boundary. A local-only rule incorrectly turns a continental
    // overriding margin into continent-continent collision even when the other
    // side is oceanic. Probe several degrees into the opposite side so the
    // continuous crust field is sampled away from the boundary itself.
    const Vec3d boundary_normal=normalized(
        tectonics_.plates()[tectonic.plate_id].seed_direction-
        tectonics_.plates()[tectonic.neighbor_plate_id].seed_direction
    );
    const Vec3d boundary_point=normalized(
        direction-boundary_normal*dot(direction,boundary_normal)
    );
    constexpr double crust_probe_offset_rad=4.0*kPi/180.0;
    const Vec3d neighbor_crust_probe=normalized(
        boundary_point*std::cos(crust_probe_offset_rad)-
        boundary_normal*std::sin(crust_probe_offset_rad)
    );
    const double neighbor_continental_score=smoothstep01(
        (
            tectonics_.sample_direction(neighbor_crust_probe).
                continental_affinity-
            0.60
        )/0.30
    );
    const double collision_weight=
        owner_continental_score*neighbor_continental_score;
    const double subduction_weight=1.0-collision_weight;
    const double convergence=std::max(0.0,tectonic.convergence);

    const std::uint32_t lo=std::min(
        tectonic.plate_id,
        tectonic.neighbor_plate_id
    );
    const std::uint32_t hi=std::max(
        tectonic.plate_id,
        tectonic.neighbor_plate_id
    );
    const std::uint64_t pair_key=
        (static_cast<std::uint64_t>(lo)<<32U) |
        static_cast<std::uint64_t>(hi);
    const bool lower_plate_subducts=deterministic_unit(
        seed_,
        fnv1a64("geology.subduction.polarity"),
        0,
        pair_key
    )<0.5;
    const std::uint32_t fallback_subducting_plate=
        lower_plate_subducts ? lo : hi;

    // At a mixed margin prefer the more oceanic side for subduction. Similar
    // crust on both sides keeps the deterministic pair-stable fallback.
    constexpr double polarity_contrast=0.25;
    bool owner_subducts=tectonic.plate_id==fallback_subducting_plate;
    if (
        owner_continental_score+polarity_contrast<
        neighbor_continental_score
    ) {
        owner_subducts=true;
    } else if (
        neighbor_continental_score+polarity_contrast<
        owner_continental_score
    ) {
        owner_subducts=false;
    }

    constexpr double trench_width_rad=1.2*kPi/180.0;
    constexpr double arc_offset_rad=3.0*kPi/180.0;
    constexpr double arc_width_rad=1.3*kPi/180.0;
    const double trench_profile=std::exp(-std::pow(
        tectonic.boundary_distance_rad/trench_width_rad,
        2.0
    ));
    const double arc_profile=std::exp(-std::pow(
        (tectonic.boundary_distance_rad-arc_offset_rad)/arc_width_rad,
        2.0
    ));

    const double trench=owner_subducts
        ? convergence*subduction_weight*trench_profile
        : 0.0;
    const double volcanic_arc=!owner_subducts
        ? convergence*subduction_weight*arc_profile
        : 0.0;

    return {
        std::clamp(trench,0.0,1.0),
        std::clamp(volcanic_arc,0.0,1.0),
        std::clamp(
            tectonic.uplift_forcing*collision_weight,
            0.0,
            1.0
        ),
        std::clamp(
            tectonic.divergence_forcing*continental_fraction,
            0.0,
            1.0
        )
    };
}

double GeologyModel::surface_elevation_m(
    const GeologyState& state,
    Vec3d direction,
    double cell_area_m2
) const {
    const TectonicSample tectonic=tectonics_.sample_direction(direction);
    const double continental_fraction=std::clamp(
        state.continental_fraction,
        0.0,
        1.0
    );
    const double continental_weight=smoothstep01(
        (continental_fraction-0.20)/0.60
    );

    const double isostatic_continent=
        -4'300.0+
        state.crust_thickness_m*
        (kMantleDensityKgM3-state.crust_density_kg_m3)/
        kMantleDensityKgM3;
    const double oceanic=ocean_floor_elevation_m(state.lithosphere_age_ma);
    double elevation=lerp(oceanic,isostatic_continent,continental_weight);

    const double area=std::max(1.0,cell_area_m2);
    const double sediment_thickness=sediment_column_thickness_m(
        state.sediment_mass_kg,
        area
    );
    // Geometric thickness follows equilibrium burial compaction while load is
    // tied to conserved solid mass. This prevents equal sediment masses from
    // creating equal thickness increments at every burial depth.
    constexpr double sediment_load_compensation=0.65;
    elevation+=sediment_thickness-
        sediment_load_compensation*
        state.sediment_mass_kg/(area*kMantleDensityKgM3);

    // Convergent boundaries are asymmetric: the selected subducting side
    // forms a trench, while the overriding side receives an inland-offset
    // volcanic arc. High-continental-fraction convergence instead becomes
    // broad collisional uplift on both sides.
    const BoundaryFeatureSample features=boundary_features(state,direction);
    elevation+=650.0*
        tectonic.uplift_forcing*
        continental_fraction;
    elevation+=1'500.0*features.collision_forcing;
    elevation-=2'500.0*
        features.trench_forcing*
        (1.0-0.4*continental_fraction);
    elevation+=(
        1'800.0+1'800.0*continental_fraction
    )*features.volcanic_arc_forcing;
    elevation-=800.0*features.rift_forcing;

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
    if (!(runoff_mm_day>0.0)) return 0.0;
    const double water_factor=std::sqrt(runoff_mm_day/3.0);
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

double GeologyModel::hillslope_transport_rate_m_per_year(
    double downhill_slope,
    double regolith_thickness_m
) const {
    if (!(downhill_slope>0.0) || !(regolith_thickness_m>0.0))
        return 0.0;

    // Reduced soil-creep closure: transport grows linearly with local slope
    // and saturates with available mobile regolith depth. The coefficient is
    // an engineering rate scale for this coarse analytical model, not an
    // Earth-calibrated hillslope diffusivity.
    constexpr double transport_depth_scale_m=1.0;
    constexpr double creep_rate_scale_m_per_year=1.0e-3;
    const double mobile_depth_factor=
        -std::expm1(-regolith_thickness_m/transport_depth_scale_m);
    return std::clamp(
        creep_rate_scale_m_per_year*
        downhill_slope*
        mobile_depth_factor,
        0.0,
        1.0e-3
    );
}

ErosionBudget GeologyModel::erode(
    GeologyState& state,
    double cell_area_m2,
    double erosion_depth_m
) const {
    ErosionBudget budget;
    if (!(erosion_depth_m>0.0) || !(cell_area_m2>0.0)) return budget;

    const double sediment_depth=sediment_column_thickness_m(
        state.sediment_mass_kg,
        cell_area_m2
    );
    const double sediment_removed_depth=std::min(
        sediment_depth,
        erosion_depth_m
    );
    // Erosion removes the shallowest part of the equilibrium column. The
    // remainder decompacts automatically when mass is converted back to
    // thickness on the next surface query.
    budget.sediment_removed_kg=std::min(
        state.sediment_mass_kg,
        sediment_mass_for_thickness_kg(
            sediment_removed_depth,
            cell_area_m2
        )
    );
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

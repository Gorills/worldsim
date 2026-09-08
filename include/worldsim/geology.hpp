#pragma once

#include "worldsim/terrain.hpp"
#include "worldsim/tectonics.hpp"

#include <cstdint>

namespace worldsim {

struct GeologyState {
    double crust_thickness_m{};
    double crust_density_kg_m3{};
    double continental_fraction{};
    double lithosphere_age_ma{};
    double sediment_mass_kg{};
    double regolith_thickness_m{};
};

struct BoundaryFeatureSample {
    double trench_forcing{};
    double volcanic_arc_forcing{};
    double collision_forcing{};
    double rift_forcing{};
};

struct ErosionBudget {
    double sediment_removed_kg{};
    double bedrock_removed_kg{};
    double crust_thickness_removed_m{};
    [[nodiscard]] double transported_mass_kg() const {
        return sediment_removed_kg+bedrock_removed_kg;
    }
};

// Stateful geological approximation layered on top of the deterministic plate
// geometry. The model intentionally resolves the processes needed to close the
// long-term terrain loop (crust production/destruction, thermal subsidence,
// isostasy, erosion and sediment mass) without pretending to be a 3-D mantle
// or lithosphere rheology solver.
class GeologyModel {
public:
    static constexpr double kMantleDensityKgM3=3300.0;

    // Generic shaley-sand/silt equilibrium-compaction closure. The retained
    // density name is the solid-grain density; bulk density emerges from the
    // depth-dependent porosity profile.
    static constexpr double kSedimentDensityKgM3=2680.0;
    static constexpr double kSedimentSurfacePorosity=0.56;
    static constexpr double kSedimentCompactionPerM=0.39e-3;

    explicit GeologyModel(std::uint64_t seed):
        seed_(seed), tectonics_(seed), terrain_(seed) {}

    [[nodiscard]] GeologyState initial_state(Vec3d direction, double cell_area_m2) const;
    void advance_tectonics(GeologyState& state, Vec3d direction, double dt_years) const;

    [[nodiscard]] double surface_elevation_m(
        const GeologyState& state,
        Vec3d direction,
        double cell_area_m2
    ) const;

    [[nodiscard]] double sediment_column_thickness_m(
        double sediment_mass_kg,
        double cell_area_m2
    ) const;

    [[nodiscard]] double sediment_mass_for_thickness_kg(
        double sediment_thickness_m,
        double cell_area_m2
    ) const;

    [[nodiscard]] BoundaryFeatureSample boundary_features(
        const GeologyState& state,
        Vec3d direction
    ) const;

    [[nodiscard]] double erosion_rate_m_per_year(
        double downhill_slope,
        double runoff_m_per_day,
        double regolith_thickness_m
    ) const;

    [[nodiscard]] double hillslope_transport_rate_m_per_year(
        double downhill_slope,
        double regolith_thickness_m
    ) const;

    [[nodiscard]] double marine_sediment_transport_rate_m_per_year(
        double downhill_slope,
        double water_depth_m,
        double sediment_thickness_m
    ) const;

    [[nodiscard]] double entrain_sediment(
        GeologyState& state,
        double cell_area_m2,
        double transport_depth_m
    ) const;

    [[nodiscard]] ErosionBudget erode(
        GeologyState& state,
        double cell_area_m2,
        double erosion_depth_m
    ) const;

    void deposit(GeologyState& state, double sediment_mass_kg) const;

    [[nodiscard]] const TectonicModel& tectonics() const { return tectonics_; }

private:
    [[nodiscard]] double ocean_floor_elevation_m(double age_ma) const;

    std::uint64_t seed_{};
    TectonicModel tectonics_;
    TerrainGenerator terrain_;
};

} // namespace worldsim

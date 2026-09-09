#pragma once

#include "worldsim/soil_carbon.hpp"

#include <array>

namespace worldsim {

inline constexpr std::array<double,3> kPlantCarbonNitrogenRatio{
    25.0,40.0,60.0
};
inline constexpr double kInitialLitterCarbonNitrogenRatio=40.0;
inline constexpr double kInitialFastSoilCarbonNitrogenRatio=14.0;
inline constexpr double kInitialSlowSoilCarbonNitrogenRatio=12.0;

struct SoilNitrogenState {
    double litter_nitrogen_kg{};
    double fast_nitrogen_kg{};
    double slow_nitrogen_kg{};
    double mineral_nitrogen_kg{};
};

struct SoilNitrogenFluxes {
    double litter_decomposed_kg{};
    double fast_decomposed_kg{};
    double slow_decomposed_kg{};
    double transferred_to_fast_kg{};
    double transferred_to_slow_kg{};
    double mineralized_kg{};
    double leached_kg{};
};

struct SoilNitrogenStep {
    SoilNitrogenState state;
    SoilNitrogenFluxes fluxes;
};

class SoilNitrogenModel {
public:
    [[nodiscard]] SoilNitrogenStep advance(
        const SoilNitrogenState& state,
        const SoilCarbonState& carbon_before,
        const SoilCarbonStep& carbon_step,
        double runoff_depth_m
    ) const;
};

[[nodiscard]] double mineral_nitrogen_fertility(
    double mineral_nitrogen_density_kg_m2
);

} // namespace worldsim

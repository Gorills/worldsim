#pragma once

namespace worldsim {

struct SoilCarbonState {
    double litter_carbon_kg{};
    double fast_carbon_kg{};
    double slow_carbon_kg{};
};

struct SoilCarbonFluxes {
    double litter_decomposed_kg{};
    double fast_decomposed_kg{};
    double slow_decomposed_kg{};
    double transferred_to_fast_kg{};
    double transferred_to_slow_kg{};
    double respired_kg{};
};

struct SoilCarbonStep {
    SoilCarbonState state;
    SoilCarbonFluxes fluxes;
};

class SoilCarbonModel {
public:
    [[nodiscard]] SoilCarbonStep advance(
        const SoilCarbonState& state,
        double temperature_k,
        double moisture_fraction,
        double dt_days
    ) const;
};

} // namespace worldsim

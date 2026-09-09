#include "worldsim/soil_nitrogen.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace worldsim {
namespace {

constexpr double mineral_nitrogen_fertility_scale_kg_m2=0.003;
constexpr double leaching_sensitivity_per_m=1.0;

void require_stock(double value) {
    if (!std::isfinite(value) || value<0.0)
        throw std::invalid_argument("invalid soil nitrogen stock");
}

double decomposed_nitrogen(
    double nitrogen,
    double carbon_before,
    double carbon_decomposed
) {
    require_stock(nitrogen);
    require_stock(carbon_before);
    require_stock(carbon_decomposed);
    if (!(carbon_before>0.0)) {
        if (carbon_decomposed>0.0)
            throw std::invalid_argument(
                "soil nitrogen carbon flux exceeds empty source"
            );
        return 0.0;
    }
    const double fraction=std::clamp(
        carbon_decomposed/carbon_before,
        0.0,
        1.0
    );
    return nitrogen*fraction;
}

double transfer_fraction(double transferred, double decomposed) {
    require_stock(transferred);
    require_stock(decomposed);
    if (!(decomposed>0.0)) return 0.0;
    if (transferred>decomposed)
        throw std::invalid_argument("invalid soil nitrogen transfer fraction");
    return transferred/decomposed;
}

} // namespace

SoilNitrogenStep SoilNitrogenModel::advance(
    const SoilNitrogenState& state,
    const SoilCarbonState& carbon_before,
    const SoilCarbonStep& carbon_step,
    double runoff_depth_m
) const {
    require_stock(state.litter_nitrogen_kg);
    require_stock(state.fast_nitrogen_kg);
    require_stock(state.slow_nitrogen_kg);
    require_stock(state.mineral_nitrogen_kg);
    require_stock(carbon_before.litter_carbon_kg);
    require_stock(carbon_before.fast_carbon_kg);
    require_stock(carbon_before.slow_carbon_kg);
    if (!std::isfinite(runoff_depth_m) || runoff_depth_m<0.0)
        throw std::invalid_argument("invalid soil nitrogen runoff depth");

    SoilNitrogenStep result;
    result.fluxes.litter_decomposed_kg=decomposed_nitrogen(
        state.litter_nitrogen_kg,
        carbon_before.litter_carbon_kg,
        carbon_step.fluxes.litter_decomposed_kg
    );
    result.fluxes.fast_decomposed_kg=decomposed_nitrogen(
        state.fast_nitrogen_kg,
        carbon_before.fast_carbon_kg,
        carbon_step.fluxes.fast_decomposed_kg
    );
    result.fluxes.slow_decomposed_kg=decomposed_nitrogen(
        state.slow_nitrogen_kg,
        carbon_before.slow_carbon_kg,
        carbon_step.fluxes.slow_decomposed_kg
    );

    result.fluxes.transferred_to_fast_kg=
        result.fluxes.litter_decomposed_kg*
        transfer_fraction(
            carbon_step.fluxes.transferred_to_fast_kg,
            carbon_step.fluxes.litter_decomposed_kg
        );
    result.fluxes.transferred_to_slow_kg=
        result.fluxes.fast_decomposed_kg*
        transfer_fraction(
            carbon_step.fluxes.transferred_to_slow_kg,
            carbon_step.fluxes.fast_decomposed_kg
        );
    result.fluxes.mineralized_kg=
        result.fluxes.litter_decomposed_kg-
            result.fluxes.transferred_to_fast_kg+
        result.fluxes.fast_decomposed_kg-
            result.fluxes.transferred_to_slow_kg+
        result.fluxes.slow_decomposed_kg;

    result.state.litter_nitrogen_kg=
        state.litter_nitrogen_kg-
        result.fluxes.litter_decomposed_kg;
    result.state.fast_nitrogen_kg=
        state.fast_nitrogen_kg-
        result.fluxes.fast_decomposed_kg+
        result.fluxes.transferred_to_fast_kg;
    result.state.slow_nitrogen_kg=
        state.slow_nitrogen_kg-
        result.fluxes.slow_decomposed_kg+
        result.fluxes.transferred_to_slow_kg;

    const double mineral_before_leaching=
        state.mineral_nitrogen_kg+
        result.fluxes.mineralized_kg;
    const double leached_fraction=
        -std::expm1(-leaching_sensitivity_per_m*runoff_depth_m);
    result.fluxes.leached_kg=
        mineral_before_leaching*std::clamp(
            leached_fraction,0.0,1.0
        );
    result.state.mineral_nitrogen_kg=
        mineral_before_leaching-result.fluxes.leached_kg;
    return result;
}

double mineral_nitrogen_fertility(
    double mineral_nitrogen_density_kg_m2
) {
    if (
        !std::isfinite(mineral_nitrogen_density_kg_m2) ||
        mineral_nitrogen_density_kg_m2<0.0
    ) {
        throw std::invalid_argument("invalid mineral nitrogen density");
    }
    return std::clamp(
        -std::expm1(
            -mineral_nitrogen_density_kg_m2/
            mineral_nitrogen_fertility_scale_kg_m2
        ),
        0.0,
        1.0
    );
}

} // namespace worldsim

#include "worldsim/soil_carbon.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace worldsim {
namespace {

constexpr double days_per_year=365.2422;
constexpr double litter_decay_per_day=1.0/(2.5*days_per_year);
constexpr double fast_decay_per_day=1.0/(4.0*days_per_year);
constexpr double slow_decay_per_day=1.0/(80.0*days_per_year);
constexpr double litter_to_fast_fraction=0.55;
constexpr double fast_to_slow_fraction=0.35;

void require_stock(double value) {
    if (!std::isfinite(value) || value<0.0)
        throw std::invalid_argument("invalid soil carbon stock");
}

double decayed_stock(
    double stock,
    double base_rate,
    double modifier,
    double dt_days
) {
    return stock*(-std::expm1(-base_rate*modifier*dt_days));
}

} // namespace

SoilCarbonStep SoilCarbonModel::advance(
    const SoilCarbonState& state,
    double temperature_k,
    double moisture_fraction,
    double dt_days
) const {
    require_stock(state.litter_carbon_kg);
    require_stock(state.fast_carbon_kg);
    require_stock(state.slow_carbon_kg);
    if (!std::isfinite(temperature_k))
        throw std::invalid_argument("invalid soil carbon temperature");
    if (!std::isfinite(moisture_fraction))
        throw std::invalid_argument("invalid soil carbon moisture");
    if (!std::isfinite(dt_days) || dt_days<0.0)
        throw std::invalid_argument("invalid soil carbon timestep");

    const double temperature_factor=std::clamp(
        std::pow(2.0,(temperature_k-283.0)/10.0),
        0.1,
        4.0
    );
    const double moisture=std::clamp(moisture_fraction,0.0,1.0);
    const double modifier=
        temperature_factor*(0.15+0.85*moisture);

    SoilCarbonStep result;
    result.fluxes.litter_decomposed_kg=decayed_stock(
        state.litter_carbon_kg,
        litter_decay_per_day,
        modifier,
        dt_days
    );
    result.fluxes.fast_decomposed_kg=decayed_stock(
        state.fast_carbon_kg,
        fast_decay_per_day,
        modifier,
        dt_days
    );
    result.fluxes.slow_decomposed_kg=decayed_stock(
        state.slow_carbon_kg,
        slow_decay_per_day,
        modifier,
        dt_days
    );
    result.fluxes.transferred_to_fast_kg=
        litter_to_fast_fraction*
        result.fluxes.litter_decomposed_kg;
    result.fluxes.transferred_to_slow_kg=
        fast_to_slow_fraction*
        result.fluxes.fast_decomposed_kg;
    result.fluxes.respired_kg=
        (1.0-litter_to_fast_fraction)*
            result.fluxes.litter_decomposed_kg+
        (1.0-fast_to_slow_fraction)*
            result.fluxes.fast_decomposed_kg+
        result.fluxes.slow_decomposed_kg;

    result.state.litter_carbon_kg=
        state.litter_carbon_kg-
        result.fluxes.litter_decomposed_kg;
    result.state.fast_carbon_kg=
        state.fast_carbon_kg-
        result.fluxes.fast_decomposed_kg+
        result.fluxes.transferred_to_fast_kg;
    result.state.slow_carbon_kg=
        state.slow_carbon_kg-
        result.fluxes.slow_decomposed_kg+
        result.fluxes.transferred_to_slow_kg;
    return result;
}

} // namespace worldsim

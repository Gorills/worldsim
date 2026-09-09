#include "worldsim/soil_carbon.hpp"
#include "worldsim/soil_nitrogen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace worldsim;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double a, double b, double relative, const char* message) {
    const double scale=std::max({1.0,std::abs(a),std::abs(b)});
    if (
        !std::isfinite(a) || !std::isfinite(b) ||
        std::abs(a-b)>relative*scale
    ) {
        throw std::runtime_error(message);
    }
}

void soil_nitrogen_closes_against_carbon_fluxes() {
    const SoilCarbonState carbon_before{100.0,200.0,1'000.0};
    const SoilCarbonStep carbon=
        SoilCarbonModel().advance(carbon_before,293.0,0.8,30.0);
    const SoilNitrogenState nitrogen_before{4.0,12.0,80.0,0.6};
    const SoilNitrogenStep nitrogen=SoilNitrogenModel().advance(
        nitrogen_before,carbon_before,carbon,0.025
    );

    const double before=
        nitrogen_before.litter_nitrogen_kg+
        nitrogen_before.fast_nitrogen_kg+
        nitrogen_before.slow_nitrogen_kg+
        nitrogen_before.mineral_nitrogen_kg;
    const double after=
        nitrogen.state.litter_nitrogen_kg+
        nitrogen.state.fast_nitrogen_kg+
        nitrogen.state.slow_nitrogen_kg+
        nitrogen.state.mineral_nitrogen_kg+
        nitrogen.fluxes.leached_kg;
    near(before,after,2.0e-15,"soil nitrogen transfer does not close");
    check(
        nitrogen.fluxes.mineralized_kg>0.0,
        "soil decomposition did not mineralize nitrogen"
    );
    check(
        nitrogen.fluxes.leached_kg>0.0,
        "positive drainage did not leach mineral nitrogen"
    );
}

void new_organic_transfer_is_staged() {
    const SoilCarbonState carbon_before{100.0,0.0,0.0};
    const SoilCarbonStep carbon=
        SoilCarbonModel().advance(carbon_before,293.0,1.0,1.0e9);
    const SoilNitrogenStep nitrogen=SoilNitrogenModel().advance(
        {4.0,0.0,0.0,0.0},
        carbon_before,
        carbon,
        0.0
    );
    near(
        nitrogen.state.fast_nitrogen_kg,
        4.0*0.55,
        2.0e-15,
        "new litter nitrogen decomposed again in the same soil step"
    );
    near(
        nitrogen.state.slow_nitrogen_kg,
        0.0,
        0.0,
        "new nitrogen crossed multiple soil pools in one step"
    );
}

void fertility_tracks_finite_mineral_stock() {
    near(
        mineral_nitrogen_fertility(0.0),0.0,0.0,
        "zero mineral nitrogen did not produce zero fertility"
    );
    check(
        mineral_nitrogen_fertility(0.006)>
        mineral_nitrogen_fertility(0.001),
        "fertility did not increase with mineral nitrogen density"
    );
    bool rejected=false;
    try {
        (void)mineral_nitrogen_fertility(
            std::numeric_limits<double>::quiet_NaN()
        );
    } catch (const std::invalid_argument&) {
        rejected=true;
    }
    check(rejected,"fertility accepted non-finite mineral nitrogen");
}

} // namespace

int main() {
    try {
        soil_nitrogen_closes_against_carbon_fluxes();
        new_organic_transfer_is_staged();
        fertility_tracks_finite_mineral_stock();
        std::cout<<"nitrogen_tests: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr<<"nitrogen_tests: FAIL: "<<error.what()<<'\n';
        return EXIT_FAILURE;
    }
}

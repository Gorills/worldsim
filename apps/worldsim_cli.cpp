#include "worldsim/simulation.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>

int main(int argc,char** argv) {
    using namespace worldsim;
    const std::uint64_t seed=argc>1 ? std::stoull(argv[1]) : 42ULL;
    auto sim=make_default_simulation(seed);
    sim->set_focus({1.0,0.0,0.0});
    sim->step(24*30);
    const auto& fs=sim->world().stores().get<FieldStore>();
    const auto temp=*sim->fields().find("climate.surface_temperature_k");
    const auto veg=*sim->fields().find("ecology.vegetation_carbon_kg");
    const auto burned=*sim->fields().find("ecology.fire_burned_area_m2");
    const auto emissions=*sim->fields().find(
        "ecology.fire_emitted_carbon_kg"
    );
    const auto danger=*sim->fields().find("ecology.fire_danger");
    double mean_temp=0.0,total_veg=0.0,total_area=0.0;
    double total_burned_area=0.0,total_fire_emissions=0.0;
    double maximum_fire_danger=0.0;
    for (CellId c:sim->world().active_cells()) {
        const double a=sim->world().topology().area_m2(c);
        mean_temp+=fs.get(c,temp)*a;
        total_area+=a;
        total_veg+=fs.get(c,veg);
        total_burned_area+=fs.get(c,burned);
        total_fire_emissions+=fs.get(c,emissions);
        maximum_fire_danger=std::max(
            maximum_fire_danger,
            fs.get(c,danger)
        );
    }
    mean_temp/=total_area;
    std::cout << "tick=" << sim->world().tick() << " cells=" << sim->world().active_cells().size()
              << " mean_temp_K=" << std::fixed << std::setprecision(2) << mean_temp
              << " vegetation_kgC=" << std::scientific << total_veg
              << " max_fire_danger=" << maximum_fire_danger
              << " fire_burned_m2=" << total_burned_area
              << " fire_emissions_kgC=" << total_fire_emissions << '\n';
    return 0;
}

#include "worldsim/simulation.hpp"
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
    double mean_temp=0.0,total_veg=0.0,total_area=0.0;
    for (CellId c:sim->world().active_cells()) {
        const double a=sim->world().topology().area_m2(c);
        mean_temp+=fs.get(c,temp)*a;
        total_area+=a;
        total_veg+=fs.get(c,veg);
    }
    mean_temp/=total_area;
    std::cout << "tick=" << sim->world().tick() << " cells=" << sim->world().active_cells().size()
              << " mean_temp_K=" << std::fixed << std::setprecision(2) << mean_temp
              << " vegetation_kgC=" << std::scientific << total_veg << '\n';
    return 0;
}

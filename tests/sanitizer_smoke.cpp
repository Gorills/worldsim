#include "worldsim/simulation.hpp"

#include <cstdlib>
#include <iostream>

using namespace worldsim;

int main() {
    try {
        SimulationConfig cfg;
        cfg.base_level=2;
        cfg.max_level=4;
        cfg.tick_seconds=3600.0;
        auto sim=make_default_simulation(0x5eedULL,cfg);
        sim->set_focus({1.0,0.25,-0.1});
        sim->step(48);
        const auto mana=*sim->fields().find("magic.mana_j");
        const auto cell=sim->world().topology().from_direction({1.0,0.25,-0.1},cfg.base_level);
        sim->schedule_field_impulse(sim->world().tick()+1,cell,mana,1.0e9);
        const auto snap=sim->save_snapshot();
        sim->set_focus({-1.0,0.1,0.2});
        sim->step(8);
        sim->load_snapshot(snap);
        sim->step(4);
        std::cout << "worldsim_sanitizer_smoke: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "worldsim_sanitizer_smoke: FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

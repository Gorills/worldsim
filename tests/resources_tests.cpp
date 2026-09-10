#include "worldsim/resources.hpp"
#include "worldsim/hydrology.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace worldsim;

namespace {

void check(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double a,double b,double relative,const char* message) {
    const double scale=std::max({1.0,std::abs(a),std::abs(b)});
    check(std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=relative*scale,message);
}

Vec3d find_resource_direction(const Simulation& simulation,ResourceKind kind,double minimum=1.0) {
    for (CellId cell:simulation.world().active_cells()) {
        const Vec3d direction=simulation.world().topology().center_unit(cell);
        if (resource_availability(simulation,direction,kind)>=minimum)
            return direction;
    }
    throw std::runtime_error("fixture has no requested resource");
}

void resource_registration_and_exact_transfer() {
    auto simulation=make_survival_simulation(
        42,SimulationConfig{2,3,3600.0}
    );
    for (std::string_view key:{
             "resources.plant_food_kg",
             "resources.wood_kg",
             "resources.stone_kg",
             "resources.metal_ore_kg"
         }) {
        check(simulation->fields().find(key).has_value(),"resource field missing");
    }

    for (ResourceKind kind:{
             ResourceKind::PlantFood,
             ResourceKind::Wood,
             ResourceKind::Stone,
             ResourceKind::MetalOre
         }) {
        const Vec3d direction=find_resource_direction(*simulation,kind,2.0);
        const double before=resource_availability(*simulation,direction,kind);
        check(player_inventory_amount(*simulation,kind)==0.0,"inventory did not start empty");
        near(collect_resource(*simulation,direction,kind,1.0),1.0,0.0,"wrong realized transfer");
        near(resource_availability(*simulation,direction,kind),before-1.0,1e-12,"source did not lose exact transfer");
        near(player_inventory_amount(*simulation,kind),1.0,1e-12,"inventory did not receive exact transfer");

        const double source_after=resource_availability(*simulation,direction,kind);
        const double inventory_after=player_inventory_amount(*simulation,kind);
        bool rejected=false;
        try {
            (void)collect_resource(*simulation,direction,kind,source_after+1.0);
        } catch (const std::exception&) {
            rejected=true;
        }
        check(rejected,"over-large resource request was not rejected");
        near(resource_availability(*simulation,direction,kind),source_after,1e-12,"rejected request mutated source");
        near(player_inventory_amount(*simulation,kind),inventory_after,1e-12,"rejected request mutated inventory");
    }
}

void water_transfer_uses_hydrology() {
    auto simulation=make_survival_simulation(
        7,SimulationConfig{2,3,3600.0}
    );
    const CellId region=*simulation->world().active_cells().begin();
    const Vec3d direction=simulation->world().topology().center_unit(region);
    auto& hydrology=simulation->world().stores().get<HydrologyStore>();
    hydrology.add_surface_water(region,2.0);
    hydrology.project(simulation->world(),simulation->fields());

    const double before=resource_availability(
        *simulation,direction,ResourceKind::FreshWater
    );
    check(before>=2000.0,"controlled surface-water source not visible to resource query");
    const double hydrology_before=hydrology.total_surface_m3();
    const double survival_water_before=total_survival_planet_water_m3(*simulation);
    near(
        collect_resource(
            *simulation,direction,ResourceKind::FreshWater,1.0
        ),
        1.0,0.0,
        "fresh-water transfer did not realize one liter"
    );
    near(
        hydrology.total_surface_m3(),
        hydrology_before-0.001,
        1e-12,
        "fresh-water collection did not debit hydrology"
    );
    near(
        player_inventory_amount(*simulation,ResourceKind::FreshWater),
        1.0,
        1e-12,
        "fresh-water inventory did not receive one liter"
    );
    near(
        total_survival_planet_water_m3(*simulation),
        survival_water_before,
        1e-13,
        "fresh-water collection changed player-plus-world water inventory"
    );
}

void resource_lod_and_snapshot_semantics() {
    auto simulation=make_survival_simulation(
        99,SimulationConfig{2,3,3600.0}
    );
    const Vec3d direction=find_resource_direction(
        *simulation,ResourceKind::Stone,10.0
    );
    const double initial=resource_availability(
        *simulation,direction,ResourceKind::Stone
    );

    simulation->set_focus(direction);
    simulation->step(1);
    near(
        resource_availability(*simulation,direction,ResourceKind::Stone),
        initial,
        1e-12,
        "resource quantity changed under refinement"
    );

    (void)collect_resource(
        *simulation,direction,ResourceKind::Stone,3.0
    );
    const auto snapshot=simulation->save_snapshot();
    const double saved_source=resource_availability(
        *simulation,direction,ResourceKind::Stone
    );
    const double saved_inventory=player_inventory_amount(
        *simulation,ResourceKind::Stone
    );

    (void)collect_resource(
        *simulation,direction,ResourceKind::Stone,2.0
    );
    simulation->load_snapshot(snapshot);
    near(
        resource_availability(*simulation,direction,ResourceKind::Stone),
        saved_source,
        1e-12,
        "snapshot did not restore depleted source"
    );
    near(
        player_inventory_amount(*simulation,ResourceKind::Stone),
        saved_inventory,
        1e-12,
        "snapshot did not restore player inventory"
    );

    simulation->clear_focus();
    simulation->step(1);
    near(
        resource_availability(*simulation,direction,ResourceKind::Stone),
        saved_source,
        1e-12,
        "resource quantity changed under coarsening"
    );
}

void renewable_resources_recover() {
    auto simulation=make_survival_simulation(
        42,SimulationConfig{2,2,3600.0}
    );
    const Vec3d direction=find_resource_direction(
        *simulation,ResourceKind::PlantFood,10.0
    );
    const double before=resource_availability(
        *simulation,direction,ResourceKind::PlantFood
    );
    const double harvest=std::min(5.0,0.25*before);
    (void)collect_resource(
        *simulation,direction,ResourceKind::PlantFood,harvest
    );
    const double depleted=resource_availability(
        *simulation,direction,ResourceKind::PlantFood
    );
    simulation->step(24);
    const double recovered=resource_availability(
        *simulation,direction,ResourceKind::PlantFood
    );
    check(recovered>depleted,"renewable plant food did not recover through world time");
}

void deterministic_player_commands() {
    auto a=make_survival_simulation(123,SimulationConfig{2,3,3600.0});
    auto b=make_survival_simulation(123,SimulationConfig{2,3,3600.0});
    const Vec3d direction=find_resource_direction(*a,ResourceKind::Stone,5.0);
    (void)collect_resource(*a,direction,ResourceKind::Stone,2.0);
    (void)collect_resource(*b,direction,ResourceKind::Stone,2.0);
    a->set_focus(direction);
    b->set_focus(direction);
    a->step(25);
    b->step(25);
    check(a->save_snapshot()==b->save_snapshot(),"same resource commands did not continue deterministically");
}

} // namespace

int main() {
    try {
        resource_registration_and_exact_transfer();
        water_transfer_uses_hydrology();
        resource_lod_and_snapshot_semantics();
        renewable_resources_recover();
        deterministic_player_commands();
        std::cout << "resource acquisition tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "resource acquisition test failure: " << error.what() << '\n';
        return 1;
    }
}

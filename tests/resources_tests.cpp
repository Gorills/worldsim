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

Vec3d find_crafting_direction(const Simulation& simulation,double minimum=10.0) {
    for (CellId cell:simulation.world().active_cells()) {
        const Vec3d direction=simulation.world().topology().center_unit(cell);
        if (
            resource_availability(simulation,direction,ResourceKind::Wood)>=minimum &&
            resource_availability(simulation,direction,ResourceKind::Stone)>=minimum
        ) {
            return direction;
        }
    }
    throw std::runtime_error("fixture has no wood-and-stone crafting region");
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
    const double local_transfer_total_before=
        hydrology_before+
        player_inventory_amount(*simulation,ResourceKind::FreshWater)/1000.0;
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
    const double local_transfer_total_after=
        hydrology.total_surface_m3()+
        player_inventory_amount(*simulation,ResourceKind::FreshWater)/1000.0;
    near(
        local_transfer_total_after,
        local_transfer_total_before,
        1e-12,
        "fresh-water source plus carried inventory did not close exactly"
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

void crafting_and_tool_use() {
    auto insufficient=make_survival_simulation(
        42,SimulationConfig{2,3,3600.0}
    );
    bool rejected=false;
    try {
        craft_stone_axe(*insufficient);
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"crafting without inputs was not rejected");
    check(!player_has_stone_axe(*insufficient),"failed crafting created a stone axe");
    near(player_inventory_amount(*insufficient,ResourceKind::Wood),0.0,0.0,"failed crafting mutated wood inventory");
    near(player_inventory_amount(*insufficient,ResourceKind::Stone),0.0,0.0,"failed crafting mutated stone inventory");

    auto simulation=make_survival_simulation(
        42,SimulationConfig{2,3,3600.0}
    );
    const Vec3d direction=find_crafting_direction(*simulation);
    const double wood_before=resource_availability(
        *simulation,direction,ResourceKind::Wood
    );

    near(
        gather_resource(*simulation,direction,ResourceKind::Wood),
        kBareHandWoodGatherKg,0.0,
        "bare-hand wood gather action has wrong yield"
    );
    near(
        gather_resource(*simulation,direction,ResourceKind::Stone),
        1.0,0.0,
        "bare-hand stone gather action has wrong yield"
    );
    near(
        player_inventory_amount(*simulation,ResourceKind::Wood),
        kStoneAxeWoodCostKg,1e-12,
        "gathered wood input missing"
    );
    near(
        player_inventory_amount(*simulation,ResourceKind::Stone),
        kStoneAxeStoneCostKg,1e-12,
        "gathered stone input missing"
    );

    craft_stone_axe(*simulation);
    check(player_has_stone_axe(*simulation),"stone axe was not created");
    near(player_inventory_amount(*simulation,ResourceKind::Wood),0.0,1e-12,"stone axe did not consume exact wood cost");
    near(player_inventory_amount(*simulation,ResourceKind::Stone),0.0,1e-12,"stone axe did not consume exact stone cost");

    const double source_after_craft=resource_availability(
        *simulation,direction,ResourceKind::Wood
    );
    near(source_after_craft,wood_before-kBareHandWoodGatherKg,1e-12,"crafting unexpectedly changed world wood source");

    const auto crafted_snapshot=simulation->save_snapshot();
    near(
        gather_resource(*simulation,direction,ResourceKind::Wood),
        kStoneAxeWoodGatherKg,0.0,
        "stone axe did not change authoritative wood gather yield"
    );
    near(
        resource_availability(*simulation,direction,ResourceKind::Wood),
        source_after_craft-kStoneAxeWoodGatherKg,1e-12,
        "stone axe gather did not debit exact world wood"
    );
    near(
        player_inventory_amount(*simulation,ResourceKind::Wood),
        kStoneAxeWoodGatherKg,1e-12,
        "stone axe gather did not credit exact wood inventory"
    );

    simulation->load_snapshot(crafted_snapshot);
    check(player_has_stone_axe(*simulation),"snapshot did not restore stone axe state");
    near(resource_availability(*simulation,direction,ResourceKind::Wood),source_after_craft,1e-12,"snapshot did not restore pre-gather wood source");
    near(player_inventory_amount(*simulation,ResourceKind::Wood),0.0,1e-12,"snapshot did not restore crafted inventory state");

    const double wood_inventory_before_duplicate=player_inventory_amount(
        *simulation,ResourceKind::Wood
    );
    const double stone_inventory_before_duplicate=player_inventory_amount(
        *simulation,ResourceKind::Stone
    );
    rejected=false;
    try {
        craft_stone_axe(*simulation);
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"duplicate stone axe craft was not rejected");
    check(player_has_stone_axe(*simulation),"duplicate craft removed existing stone axe");
    near(player_inventory_amount(*simulation,ResourceKind::Wood),wood_inventory_before_duplicate,1e-12,"duplicate craft mutated wood inventory");
    near(player_inventory_amount(*simulation,ResourceKind::Stone),stone_inventory_before_duplicate,1e-12,"duplicate craft mutated stone inventory");

    simulation->set_focus(direction);
    simulation->step(1);
    check(player_has_stone_axe(*simulation),"refinement changed non-spatial tool state");
    simulation->clear_focus();
    simulation->step(1);
    check(player_has_stone_axe(*simulation),"coarsening changed non-spatial tool state");
}

void deterministic_player_commands() {
    auto a=make_survival_simulation(123,SimulationConfig{2,3,3600.0});
    auto b=make_survival_simulation(123,SimulationConfig{2,3,3600.0});
    const Vec3d direction=find_crafting_direction(*a);
    for (Simulation* simulation:{a.get(),b.get()}) {
        (void)gather_resource(*simulation,direction,ResourceKind::Wood);
        (void)gather_resource(*simulation,direction,ResourceKind::Stone);
        craft_stone_axe(*simulation);
        (void)gather_resource(*simulation,direction,ResourceKind::Wood);
        simulation->set_focus(direction);
        simulation->step(25);
    }
    check(a->save_snapshot()==b->save_snapshot(),"same gather/craft commands did not continue deterministically");
}

} // namespace

int main() {
    try {
        resource_registration_and_exact_transfer();
        water_transfer_uses_hydrology();
        resource_lod_and_snapshot_semantics();
        renewable_resources_recover();
        crafting_and_tool_use();
        deterministic_player_commands();
        std::cout << "resource acquisition and crafting tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "resource acquisition/crafting test failure: " << error.what() << '\n';
        return 1;
    }
}

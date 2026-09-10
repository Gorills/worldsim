#include "worldsim/campsite.hpp"
#include "worldsim/resources.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace worldsim;

namespace {

void check(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double a,double b,double relative,const char* message) {
    const double scale=std::max({1.0,std::abs(a),std::abs(b)});
    check(
        std::isfinite(a) && std::isfinite(b) &&
        std::abs(a-b)<=relative*scale,
        message
    );
}

Vec3d find_campsite_direction(
    const Simulation& simulation,
    double minimum_wood_kg=16.0,
    double minimum_stone_kg=8.0
) {
    for (CellId cell:simulation.world().active_cells()) {
        const Vec3d direction=simulation.world().topology().center_unit(cell);
        if (!campsite_site_suitable(simulation,direction)) continue;
        if (
            resource_availability(simulation,direction,ResourceKind::Wood)>=minimum_wood_kg &&
            resource_availability(simulation,direction,ResourceKind::Stone)>=minimum_stone_kg
        ) {
            return direction;
        }
    }
    throw std::runtime_error("fixture has no suitable campsite with materials");
}

Vec3d find_unsuitable_direction(const Simulation& simulation) {
    for (CellId cell:simulation.world().active_cells()) {
        const Vec3d direction=simulation.world().topology().center_unit(cell);
        if (!campsite_site_suitable(simulation,direction)) return direction;
    }
    throw std::runtime_error("fixture has no unsuitable campsite region");
}

void gather_crafting_and_build_inputs(Simulation& simulation,Vec3d direction) {
    // Start from the same authoritative player path as the playable client:
    // gather 1 kg wood + 1 kg stone, craft the axe, then use its 3 kg wood
    // yield to gather the construction/fuel stock.
    near(
        gather_resource(simulation,direction,ResourceKind::Wood),
        1.0,0.0,
        "initial wood gather did not realize one kilogram"
    );
    near(
        gather_resource(simulation,direction,ResourceKind::Stone),
        1.0,0.0,
        "initial stone gather did not realize one kilogram"
    );
    craft_stone_axe(simulation);

    for (int i=0;i<3;++i)
        near(
            gather_resource(simulation,direction,ResourceKind::Wood),
            3.0,0.0,
            "axe wood gather did not realize three kilograms"
        );
    for (int i=0;i<4;++i)
        near(
            gather_resource(simulation,direction,ResourceKind::Stone),
            1.0,0.0,
            "stone gather did not realize one kilogram"
        );
}

void construction_and_fuel_are_exact_and_atomic() {
    auto simulation=make_survival_simulation(
        42,SimulationConfig{2,3,3600.0}
    );
    const Vec3d direction=find_campsite_direction(*simulation);
    const CellId site=campsite_site_cell(*simulation,direction);
    check(site.level()==kGameplaySiteLevel,"campsite does not use fixed gameplay level");
    check(!campsite_state(*simulation,direction).shelter,"campsite did not start empty");

    bool rejected=false;
    try {
        build_basic_shelter(*simulation,direction);
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"shelter without axe/materials was not rejected");
    check(!campsite_state(*simulation,direction).shelter,"failed shelter build created state");

    gather_crafting_and_build_inputs(*simulation,direction);
    near(player_inventory_amount(*simulation,ResourceKind::Wood),9.0,1e-12,"wrong pre-build wood inventory");
    near(player_inventory_amount(*simulation,ResourceKind::Stone),4.0,1e-12,"wrong pre-build stone inventory");

    build_basic_shelter(*simulation,direction);
    CampsiteState state=campsite_state(*simulation,direction);
    check(state.shelter,"shelter build did not persist");
    near(player_inventory_amount(*simulation,ResourceKind::Wood),3.0,1e-12,"shelter did not debit exact wood cost");
    near(player_inventory_amount(*simulation,ResourceKind::Stone),2.0,1e-12,"shelter did not debit exact stone cost");

    const double wood_before_duplicate=player_inventory_amount(*simulation,ResourceKind::Wood);
    const double stone_before_duplicate=player_inventory_amount(*simulation,ResourceKind::Stone);
    rejected=false;
    try {
        build_basic_shelter(*simulation,direction);
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"duplicate shelter build was not rejected");
    near(player_inventory_amount(*simulation,ResourceKind::Wood),wood_before_duplicate,1e-12,"duplicate shelter mutated wood inventory");
    near(player_inventory_amount(*simulation,ResourceKind::Stone),stone_before_duplicate,1e-12,"duplicate shelter mutated stone inventory");

    build_campfire(*simulation,direction);
    state=campsite_state(*simulation,direction);
    check(state.campfire,"campfire build did not persist");
    near(player_inventory_amount(*simulation,ResourceKind::Stone),0.0,1e-12,"campfire did not debit exact stone cost");

    rejected=false;
    try {
        add_campfire_fuel(*simulation,direction,4.0);
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"over-large campfire fuel transfer was not rejected");
    near(player_inventory_amount(*simulation,ResourceKind::Wood),3.0,1e-12,"rejected fuel transfer mutated inventory");
    near(campsite_state(*simulation,direction).campfire_fuel_kg,0.0,1e-12,"rejected fuel transfer mutated campfire");

    add_campfire_fuel(*simulation,direction,1.0);
    near(player_inventory_amount(*simulation,ResourceKind::Wood),2.0,1e-12,"fuel transfer did not debit carried wood exactly");
    near(campsite_state(*simulation,direction).campfire_fuel_kg,1.0,1e-12,"fuel transfer did not credit campfire exactly");
}

void campfire_burn_snapshot_lod_and_wildfire_separation() {
    auto simulation=make_survival_simulation(
        42,SimulationConfig{2,3,3600.0}
    );
    const Vec3d direction=find_campsite_direction(*simulation);
    gather_crafting_and_build_inputs(*simulation,direction);
    build_basic_shelter(*simulation,direction);
    build_campfire(*simulation,direction);
    add_campfire_fuel(*simulation,direction,1.0);

    const auto fire_active_id=simulation->fields().find("ecology.fire_active_area_m2");
    check(fire_active_id.has_value(),"natural fire authority field missing");
    auto natural_fire_total=[&]() {
        double total=0.0;
        const auto& fields=simulation->world().stores().get<FieldStore>();
        for (CellId cell:simulation->world().active_cells())
            total+=fields.get(cell,*fire_active_id);
        return total;
    };

    const double wildfire_before=natural_fire_total();
    set_campfire_lit(*simulation,direction,true);
    near(natural_fire_total(),wildfire_before,0.0,"lighting campfire directly mutated wildfire state");

    simulation->step(1);
    CampsiteState state=campsite_state(*simulation,direction);
    check(state.campfire_lit,"campfire unexpectedly extinguished with fuel remaining");
    near(state.campfire_fuel_kg,0.75,1e-12,"campfire did not burn 0.25 kg per simulated hour");

    const auto snapshot=simulation->save_snapshot();
    const CellId site_before_lod=campsite_site_cell(*simulation,direction);
    simulation->set_focus(direction);
    simulation->step(1);
    check(campsite_site_cell(*simulation,direction)==site_before_lod,"refinement changed campsite identity");
    check(campsite_state(*simulation,direction).shelter,"refinement lost shelter state");
    simulation->clear_focus();
    simulation->step(1);
    check(campsite_site_cell(*simulation,direction)==site_before_lod,"coarsening changed campsite identity");
    check(campsite_state(*simulation,direction).campfire,"coarsening lost campfire state");

    simulation->step(10);
    state=campsite_state(*simulation,direction);
    near(state.campfire_fuel_kg,0.0,1e-12,"campfire fuel became non-zero after exhaustion");
    check(!state.campfire_lit,"campfire did not extinguish at zero fuel");

    simulation->load_snapshot(snapshot);
    state=campsite_state(*simulation,direction);
    check(state.shelter && state.campfire && state.campfire_lit,"snapshot did not restore campsite flags");
    near(state.campfire_fuel_kg,0.75,1e-12,"snapshot did not restore campfire fuel");
}

void unsuitable_sites_reject_before_mutation() {
    auto simulation=make_survival_simulation(
        42,SimulationConfig{2,3,3600.0}
    );
    const Vec3d gather_direction=find_campsite_direction(*simulation);
    const Vec3d unsuitable=find_unsuitable_direction(*simulation);
    gather_crafting_and_build_inputs(*simulation,gather_direction);

    const double wood_before=player_inventory_amount(*simulation,ResourceKind::Wood);
    const double stone_before=player_inventory_amount(*simulation,ResourceKind::Stone);
    bool rejected=false;
    try {
        build_basic_shelter(*simulation,unsuitable);
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"unsuitable shelter site was accepted");
    near(player_inventory_amount(*simulation,ResourceKind::Wood),wood_before,1e-12,"unsuitable site mutated wood inventory");
    near(player_inventory_amount(*simulation,ResourceKind::Stone),stone_before,1e-12,"unsuitable site mutated stone inventory");
    check(!campsite_state(*simulation,unsuitable).shelter,"unsuitable site created shelter state");
}

void deterministic_campsite_commands() {
    auto a=make_survival_simulation(123,SimulationConfig{2,3,3600.0});
    auto b=make_survival_simulation(123,SimulationConfig{2,3,3600.0});
    const Vec3d direction=find_campsite_direction(*a);
    for (Simulation* simulation:{a.get(),b.get()}) {
        gather_crafting_and_build_inputs(*simulation,direction);
        build_basic_shelter(*simulation,direction);
        build_campfire(*simulation,direction);
        add_campfire_fuel(*simulation,direction,1.0);
        set_campfire_lit(*simulation,direction,true);
        simulation->set_focus(direction);
        simulation->step(5);
    }
    check(a->save_snapshot()==b->save_snapshot(),"same campsite commands did not continue deterministically");
}

} // namespace

int main() {
    try {
        construction_and_fuel_are_exact_and_atomic();
        campfire_burn_snapshot_lod_and_wildfire_separation();
        unsuitable_sites_reject_before_mutation();
        deterministic_campsite_commands();
        std::cout << "shelter and controlled campfire tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shelter/campfire test failure: " << error.what() << '\n';
        return 1;
    }
}

#include "worldsim/simulation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace worldsim;

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double a, double b, double relative, const char* message) {
    const double scale=std::max({1.0,std::abs(a),std::abs(b)});
    if (std::abs(a-b)>relative*scale)
        throw std::runtime_error(message);
}

void clear_fauna(Simulation& sim) {
    auto& cohorts=sim.world().stores().get<CohortStore>();
    for (CellId cell:sim.world().active_cells()) {
        for (auto& ref:cohorts.in_cell(cell))
            ref.get().count=0.0;
    }
}

void clear_plants(Simulation& sim) {
    auto& fields=sim.world().stores().get<FieldStore>();
    const std::array<FieldId,4> plant_fields{
        *sim.fields().find("ecology.grass_carbon_kg"),
        *sim.fields().find("ecology.shrub_carbon_kg"),
        *sim.fields().find("ecology.tree_carbon_kg"),
        *sim.fields().find("ecology.vegetation_carbon_kg")
    };
    for (CellId cell:sim.world().active_cells()) {
        for (FieldId field:plant_fields)
            fields.set(cell,field,0.0);
    }
}

std::pair<CellId,CellId> find_land_pair(
    Simulation& sim,
    double minimum_land_fraction=0.80
) {
    const auto land=*sim.fields().find("geography.land_fraction");
    const auto& fields=sim.world().stores().get<FieldStore>();
    for (CellId source:sim.world().active_cells()) {
        if (fields.get(source,land)<minimum_land_fraction) continue;
        for (const auto& side:sim.world().active_neighbors4(source)) {
            if (side.size()!=1U) continue;
            const CellId target=side.front().cell;
            if (fields.get(target,land)>=minimum_land_fraction)
                return {source,target};
        }
    }
    throw std::runtime_error(
        "fauna migration fixture found no neighboring land cells"
    );
}

void set_grass_density(
    Simulation& sim,
    CellId cell,
    double density_kg_m2
) {
    auto& fields=sim.world().stores().get<FieldStore>();
    const auto land=*sim.fields().find("geography.land_fraction");
    const auto grass=*sim.fields().find("ecology.grass_carbon_kg");
    const auto total=*sim.fields().find("ecology.vegetation_carbon_kg");
    const double effective_area=
        sim.world().topology().area_m2(cell)*fields.get(cell,land);
    const double carbon=density_kg_m2*effective_area;
    fields.set(cell,grass,carbon);
    fields.set(cell,total,carbon);
}

double lineage_count_in_cell(
    const CohortStore& cohorts,
    CellId cell,
    std::uint64_t lineage_id
) {
    double total=0.0;
    for (const auto& ref:cohorts.in_cell(cell)) {
        const Cohort& cohort=ref.get();
        if (cohort.lineage_id==lineage_id)
            total+=cohort.count;
    }
    return total;
}

void test_indexed_transfer_conserves_population() {
    FieldRegistry registry;
    registry.freeze();
    WorldState world(77);
    auto& cohorts=world.stores().emplace<CohortStore>();
    world.initialize_cover(0);

    const CellId source=CellId::make(0,0,0,0);
    const CellId target=CellId::make(1,0,0,0);
    Cohort& original=cohorts.add({
        0,0,source,42,1,25.0,2.0,0.5
    });
    const std::uint64_t source_id=original.id;
    const std::uint64_t lineage_id=original.lineage_id;
    const double before=cohorts.total_count();

    const std::uint64_t destination_id=
        cohorts.transfer_count(source_id,target,10.0);
    near(
        cohorts.total_count(),
        before,
        1.0e-15,
        "indexed cohort transfer changed total population"
    );
    near(
        lineage_count_in_cell(cohorts,source,lineage_id),
        15.0,
        1.0e-15,
        "indexed cohort transfer did not debit source"
    );
    near(
        lineage_count_in_cell(cohorts,target,lineage_id),
        10.0,
        1.0e-15,
        "indexed cohort transfer did not credit destination"
    );
    check(
        cohorts.in_cell(target).front().get().id==destination_id,
        "indexed cohort transfer returned wrong destination id"
    );

    cohorts.transfer_count(source_id,target,5.0);
    check(
        cohorts.in_cell(target).size()==1U,
        "repeated transfer duplicated a compatible destination cohort"
    );
    near(
        lineage_count_in_cell(cohorts,target,lineage_id),
        15.0,
        1.0e-15,
        "repeated transfer did not merge into destination cohort"
    );
    near(
        cohorts.total_count(),
        before,
        1.0e-15,
        "repeated indexed transfer changed total population"
    );
}

void test_uniform_habitat_selection() {
    SimulationConfig config;
    config.base_level=2;
    config.max_level=2;
    config.tick_seconds=3600.0;
    constexpr std::uint64_t seed=5151;

    auto herbivore_world=make_default_simulation(seed,config);
    clear_fauna(*herbivore_world);
    clear_plants(*herbivore_world);
    auto& herbivore_cohorts=
        herbivore_world->world().stores().get<CohortStore>();
    const auto [herb_source,herb_target]=
        find_land_pair(*herbivore_world);
    set_grass_density(*herbivore_world,herb_target,1.0);

    Cohort& herbivore=herbivore_cohorts.add({
        0,0,herb_source,9101,1,100.0,35.0,2.0
    });
    const std::uint64_t herb_lineage=herbivore.lineage_id;
    herbivore_world->step(24);

    const double herb_source_count=lineage_count_in_cell(
        herbivore_cohorts,
        herb_source,
        herb_lineage
    );
    const double herb_target_count=lineage_count_in_cell(
        herbivore_cohorts,
        herb_target,
        herb_lineage
    );
    check(
        herb_source_count>0.0 &&
        herb_target_count>0.0 &&
        herb_target_count<herb_source_count,
        "herbivore did not partially redistribute toward better forage"
    );

    std::size_t herb_occupied_cells=0;
    for (CellId cell:herbivore_world->world().active_cells()) {
        if (
            lineage_count_in_cell(
                herbivore_cohorts,
                cell,
                herb_lineage
            )>0.0
        ) {
            ++herb_occupied_cells;
        }
    }
    check(
        herb_occupied_cells==2U,
        "new herbivore arrival moved again within the same fauna tick"
    );

    auto carnivore_world=make_default_simulation(seed,config);
    clear_fauna(*carnivore_world);
    clear_plants(*carnivore_world);
    auto& carnivore_fields=
        carnivore_world->world().stores().get<FieldStore>();
    auto& carnivore_cohorts=
        carnivore_world->world().stores().get<CohortStore>();
    const auto [carn_source,carn_target]=
        find_land_pair(*carnivore_world);
    set_grass_density(*carnivore_world,carn_target,1.0);

    const auto land=*carnivore_world->fields().find(
        "geography.land_fraction"
    );
    const double target_effective_area=
        carnivore_world->world().topology().area_m2(carn_target)*
        carnivore_fields.get(carn_target,land);

    // Carnivore habitat quality is defined from prey biomass density. Scale
    // the fixture by cell area instead of using an absolute animal count so
    // the regression has the same ecological meaning at different LODs.
    constexpr double prey_density_kg_m2=1.0e-4;
    constexpr double prey_body_mass_kg=35.0;
    const double prey_count=
        prey_density_kg_m2*target_effective_area/prey_body_mass_kg;
    carnivore_cohorts.add({
        0,0,carn_target,9201,1,prey_count,prey_body_mass_kg,2.0
    });
    Cohort& carnivore=carnivore_cohorts.add({
        0,0,carn_source,9202,2,50.0,70.0,4.0
    });
    const std::uint64_t carn_lineage=carnivore.lineage_id;
    carnivore_world->step(24);

    const double carn_source_count=lineage_count_in_cell(
        carnivore_cohorts,
        carn_source,
        carn_lineage
    );
    const double carn_target_count=lineage_count_in_cell(
        carnivore_cohorts,
        carn_target,
        carn_lineage
    );
    check(
        carn_source_count>0.0 &&
        carn_target_count>0.0 &&
        carn_target_count<carn_source_count,
        "carnivore did not partially redistribute toward prey"
    );

    std::size_t carn_occupied_cells=0;
    for (CellId cell:carnivore_world->world().active_cells()) {
        if (
            lineage_count_in_cell(
                carnivore_cohorts,
                cell,
                carn_lineage
            )>0.0
        ) {
            ++carn_occupied_cells;
        }
    }
    check(
        carn_occupied_cells==2U,
        "new carnivore arrival moved again within the same fauna tick"
    );
}

void test_migration_resolves_refined_neighbor_region() {
    SimulationConfig config;
    config.base_level=2;
    config.max_level=3;
    config.tick_seconds=3600.0;
    constexpr std::uint64_t seed=5151;

    auto sim=make_default_simulation(seed,config);
    clear_fauna(*sim);
    clear_plants(*sim);
    const auto [source,target_region]=find_land_pair(*sim);

    // Level-2 neighboring cell centers are far enough apart that focusing the
    // target keeps it at level 3 while the source remains level 2. Refine the
    // destination explicitly before installing the controlled habitat state.
    sim->world().refine(target_region);
    sim->set_focus(sim->world().topology().center_unit(target_region));
    clear_fauna(*sim);
    clear_plants(*sim);

    check(
        sim->world().active_cells().contains(source),
        "mixed-LOD migration fixture unexpectedly refined source"
    );
    check(
        !sim->world().active_cells().contains(target_region),
        "mixed-LOD migration fixture did not refine target region"
    );

    const auto sides=sim->world().active_neighbors4(source);
    const std::vector<ActiveCoverPart>* refined_side=nullptr;
    for (const auto& side:sides) {
        if (
            side.size()==4U &&
            std::all_of(
                side.begin(),
                side.end(),
                [&](const ActiveCoverPart& part) {
                    return part.cell.parent()==target_region;
                }
            )
        ) {
            refined_side=&side;
            break;
        }
    }
    check(
        refined_side!=nullptr,
        "source did not resolve refined destination as active-cover neighbors"
    );

    for (const ActiveCoverPart& part:*refined_side)
        set_grass_density(*sim,part.cell,1.0);

    auto& cohorts=sim->world().stores().get<CohortStore>();
    Cohort& mover=cohorts.add({
        0,0,source,9301,1,100.0,35.0,2.0
    });
    const std::uint64_t lineage=mover.lineage_id;
    sim->step(24);

    check(
        sim->world().active_cells().contains(source),
        "source LOD changed during mixed-LOD migration regression"
    );
    check(
        !sim->world().active_cells().contains(target_region),
        "refined destination collapsed during migration regression"
    );

    double arrived=0.0;
    std::size_t occupied_children=0;
    for (CellId child:target_region.children()) {
        check(
            sim->world().active_cells().contains(child),
            "migration destination is not an active refined child"
        );
        const double count=lineage_count_in_cell(cohorts,child,lineage);
        arrived+=count;
        occupied_children+=count>0.0 ? 1U : 0U;
    }
    check(
        lineage_count_in_cell(cohorts,source,lineage)>0.0 &&
        arrived>0.0,
        "herbivore did not cross a coarse-to-fine active-cover boundary"
    );
    check(
        occupied_children>=2U,
        "mixed-LOD migration collapsed a refined neighbor region to one leaf"
    );
    cohorts.validate_active_cover(sim->world().active_cells());
}

void test_snapshot_epoch_current() {
    auto sim=make_default_simulation(6060);
    const auto snapshot=sim->save_snapshot();
    check(snapshot.size()>11U,"snapshot header is unexpectedly short");
    check(
        snapshot[8]==std::byte{18},
        "unexpected authoritative snapshot epoch"
    );

    auto legacy=snapshot;
    legacy[8]=std::byte{17};
    bool rejected=false;
    try {
        auto restored=make_default_simulation(6060);
        restored->load_snapshot(legacy);
    } catch (const std::runtime_error&) {
        rejected=true;
    }
    check(rejected,"snapshot v17 was accepted by the climate-v2 model");

    auto restored=make_default_simulation(6060);
    restored->load_snapshot(snapshot);
    check(
        restored->save_snapshot()==snapshot,
        "fauna v2 snapshot roundtrip changed authoritative state"
    );
}

} // namespace

int main() {
    try {
        test_indexed_transfer_conserves_population();
        test_uniform_habitat_selection();
        test_migration_resolves_refined_neighbor_region();
        test_snapshot_epoch_current();
        std::cout << "fauna_v2_tests: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "fauna_v2_tests: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

#include "worldsim/climate.hpp"
#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"
#include "worldsim/soil_nitrogen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace worldsim;
namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double a, double b, double relative, const char* message) {
    const double scale=std::max({1.0,std::abs(a),std::abs(b)});
    if (!std::isfinite(a) || !std::isfinite(b) ||
        std::abs(a-b)>relative*scale) {
        throw std::runtime_error(message);
    }
}

FieldId field(const Simulation& simulation, std::string_view key) {
    const auto id=simulation.fields().find(key);
    if (!id) throw std::runtime_error("missing fire fixture field");
    return *id;
}

void run_fire(Simulation& simulation, double dt_days=1.0) {
    Scheduler scheduler;
    GeographyModule().register_systems(scheduler,simulation.fields());
    ClimateModule().register_systems(scheduler,simulation.fields());
    MagicModule().register_systems(scheduler,simulation.fields());
    HydrologyModule().register_systems(scheduler,simulation.fields());
    EcologyModule().register_systems(scheduler,simulation.fields());
    scheduler.finalize();
    SystemContext context{
        simulation.world(),simulation.fields(),dt_days
    };
    for (ISimSystem* system:scheduler.order()) {
        if (system->id()=="ecology.fire") {
            system->step(context);
            return;
        }
    }
    throw std::runtime_error("fire system is absent");
}

void clear_and_dry(Simulation& simulation) {
    auto& fields=simulation.world().stores().get<FieldStore>();
    const std::array<FieldId,3> pft{
        field(simulation,"ecology.grass_carbon_kg"),
        field(simulation,"ecology.shrub_carbon_kg"),
        field(simulation,"ecology.tree_carbon_kg")
    };
    for (CellId cell:simulation.world().active_cells()) {
        fields.set(cell,field(simulation,"geography.land_fraction"),1.0);
        fields.set(
            cell,field(simulation,"geology.regolith_thickness_m"),1.0
        );
        fields.set(cell,field(simulation,"hydrology.soil_water_m3"),0.0);
        fields.set(cell,field(simulation,"hydrology.flooded_fraction"),0.0);
        fields.set(
            cell,field(simulation,"climate.surface_temperature_k"),305.0
        );
        fields.set(
            cell,field(simulation,"climate.precipitation_mm_day"),0.0
        );
        fields.set(cell,field(simulation,"climate.relative_humidity"),0.10);
        fields.set(cell,field(simulation,"climate.wind_east_m_s"),8.0);
        fields.set(cell,field(simulation,"climate.wind_north_m_s"),1.0);
        for (FieldId id:pft) fields.set(cell,id,0.0);
        for (std::string_view key:{
                 "ecology.vegetation_carbon_kg",
                 "ecology.litter_carbon_kg",
                 "ecology.fire_active_area_m2",
                 "ecology.fire_active_fraction",
                 "ecology.fire_danger",
                 "ecology.fire_burned_fraction",
                 "ecology.fire_burned_area_m2",
                 "ecology.fire_emitted_carbon_kg",
                 "ecology.fire_emission_kg_day",
                 "ecology.fire_emitted_nitrogen_kg",
                 "ecology.pyrogenic_carbon_kg",
                 "ecology.litter_nitrogen_kg",
                 "ecology.mineral_nitrogen_kg",
                 "ecology.grass_nitrogen_kg",
                 "ecology.shrub_nitrogen_kg",
                 "ecology.tree_nitrogen_kg",
                 "ecology.vegetation_nitrogen_kg"
             }) {
            fields.set(cell,field(simulation,key),0.0);
        }
    }
}

double fire_accounted_carbon(
    const Simulation& simulation,
    CellId cell
) {
    const auto& fields=simulation.world().stores().get<FieldStore>();
    double total=0.0;
    for (std::string_view key:{
             "ecology.grass_carbon_kg",
             "ecology.shrub_carbon_kg",
             "ecology.tree_carbon_kg",
             "ecology.litter_carbon_kg",
             "ecology.fire_emitted_carbon_kg",
             "ecology.pyrogenic_carbon_kg"
         }) {
        total+=fields.get(cell,field(simulation,key));
    }
    return total;
}

void set_fuel(Simulation& simulation, CellId cell) {
    auto& fields=simulation.world().stores().get<FieldStore>();
    const double area=simulation.world().topology().area_m2(cell);
    fields.set(
        cell,field(simulation,"ecology.grass_carbon_kg"),0.60*area
    );
    fields.set(
        cell,field(simulation,"ecology.shrub_carbon_kg"),0.35*area
    );
    fields.set(
        cell,field(simulation,"ecology.tree_carbon_kg"),1.20*area
    );
    fields.set(
        cell,field(simulation,"ecology.vegetation_carbon_kg"),2.15*area
    );
    fields.set(
        cell,field(simulation,"ecology.grass_nitrogen_kg"),
        0.60*area/kPlantCarbonNitrogenRatio[0]
    );
    fields.set(
        cell,field(simulation,"ecology.shrub_nitrogen_kg"),
        0.35*area/kPlantCarbonNitrogenRatio[1]
    );
    fields.set(
        cell,field(simulation,"ecology.tree_nitrogen_kg"),
        1.20*area/kPlantCarbonNitrogenRatio[2]
    );
    fields.set(
        cell,field(simulation,"ecology.vegetation_nitrogen_kg"),
        0.60*area/kPlantCarbonNitrogenRatio[0]+
        0.35*area/kPlantCarbonNitrogenRatio[1]+
        1.20*area/kPlantCarbonNitrogenRatio[2]
    );
    fields.set(
        cell,field(simulation,"ecology.litter_carbon_kg"),0.45*area
    );
    fields.set(
        cell,field(simulation,"ecology.litter_nitrogen_kg"),
        0.45*area/kInitialLitterCarbonNitrogenRatio
    );
}

void ignite(Simulation& simulation, CellId cell, double fraction) {
    auto& fields=simulation.world().stores().get<FieldStore>();
    const double land_area=
        simulation.world().topology().area_m2(cell)*
        fields.get(cell,field(simulation,"geography.land_fraction"));
    fields.set(
        cell,
        field(simulation,"ecology.fire_active_area_m2"),
        land_area*fraction
    );
    fields.set(
        cell,
        field(simulation,"ecology.fire_active_fraction"),
        fraction
    );
}

void controlled_fire_closes_carbon() {
    auto simulation=make_default_simulation(
        7001,SimulationConfig{1,1,3'600.0}
    );
    clear_and_dry(*simulation);
    const CellId cell=*simulation->world().active_cells().begin();
    set_fuel(*simulation,cell);
    auto& fields=simulation->world().stores().get<FieldStore>();
    ignite(*simulation,cell,0.08);

    const double before=fire_accounted_carbon(*simulation,cell);
    const double nitrogen_before=total_ecology_nitrogen_accounted_kg(
        simulation->world(),simulation->fields()
    );
    run_fire(*simulation);
    const double after=fire_accounted_carbon(*simulation,cell);
    near(before,after,2.0e-15,"fire carbon transfers do not close");
    near(
        nitrogen_before,
        total_ecology_nitrogen_accounted_kg(
            simulation->world(),simulation->fields()
        ),
        3.0e-15,
        "fire nitrogen transfers do not close"
    );
    check(
        fields.get(
            cell,field(*simulation,"ecology.fire_burned_fraction")
        )>0.0,
        "controlled active fire burned no area"
    );
    check(
        fields.get(
            cell,field(*simulation,"ecology.fire_emitted_carbon_kg")
        )>0.0,
        "controlled fire produced no emission carbon"
    );
    near(
        fields.get(
            cell,field(*simulation,"ecology.fire_emission_kg_day")
        ),
        fields.get(
            cell,field(*simulation,"ecology.fire_emitted_carbon_kg")
        ),
        1.0e-15,
        "fire emission rate disagrees with its one-day cumulative ledger"
    );
    check(
        fields.get(
            cell,field(*simulation,"ecology.pyrogenic_carbon_kg")
        )>0.0,
        "controlled fire produced no pyrogenic carbon"
    );
    check(
        fields.get(
            cell,field(*simulation,"ecology.fire_emitted_nitrogen_kg")
        )>0.0,
        "controlled fire produced no nitrogen boundary emission"
    );
    near(
        fields.get(
            cell,field(*simulation,"ecology.fire_nitrogen_emission_kg_day")
        ),
        fields.get(
            cell,field(*simulation,"ecology.fire_emitted_nitrogen_kg")
        ),
        1.0e-15,
        "fire nitrogen rate disagrees with its one-day cumulative ledger"
    );
    near(
        fields.get(
            cell,field(*simulation,"ecology.vegetation_carbon_kg")
        ),
        fields.get(
            cell,field(*simulation,"ecology.grass_carbon_kg")
        )+
        fields.get(
            cell,field(*simulation,"ecology.shrub_carbon_kg")
        )+
        fields.get(
            cell,field(*simulation,"ecology.tree_carbon_kg")
        ),
        1.0e-15,
        "fire left aggregate vegetation out of sync with PFT pools"
    );
}


void burn_cap_scales_with_elapsed_time() {
    const auto run_fixture=[](double dt_days) {
        auto simulation=make_default_simulation(
            7010,SimulationConfig{1,1,3'600.0}
        );
        clear_and_dry(*simulation);
        const CellId cell=*simulation->world().active_cells().begin();
        set_fuel(*simulation,cell);
        ignite(*simulation,cell,0.50);

        run_fire(*simulation,dt_days);
        return simulation->world().stores().get<FieldStore>().get(
            cell,
            field(*simulation,"ecology.fire_burned_fraction")
        );
    };

    const double half_day_burned=run_fixture(0.5);
    const double full_day_burned=run_fixture(1.0);
    check(
        half_day_burned>0.0 && full_day_burned>0.0,
        "fire timestep fixture did not burn"
    );
    near(
        2.0*half_day_burned,
        full_day_burned,
        1.0e-12,
        "daily fire burn cap did not scale with elapsed time"
    );
}

void persistence_scales_with_elapsed_time() {
    const auto run_fixture=[](double dt_days) {
        auto simulation=make_default_simulation(
            7011,SimulationConfig{1,1,3'600.0}
        );
        clear_and_dry(*simulation);
        const CellId cell=*simulation->world().active_cells().begin();
        set_fuel(*simulation,cell);
        ignite(*simulation,cell,0.08);

        run_fire(*simulation,dt_days);
        const auto& fields=
            simulation->world().stores().get<FieldStore>();
        const double burned=fields.get(
            cell,
            field(*simulation,"ecology.fire_burned_fraction")
        );
        const double active=fields.get(
            cell,
            field(*simulation,"ecology.fire_active_fraction")
        );
        const double danger=fields.get(
            cell,
            field(*simulation,"ecology.fire_danger")
        );
        check(
            burned>0.0 && active>0.0 && danger>0.0,
            "fire persistence fixture did not remain active"
        );
        return std::array<double,3>{burned,active,danger};
    };

    const auto half_day=run_fixture(0.5);
    const auto full_day=run_fixture(1.0);
    near(
        half_day[2],
        full_day[2],
        1.0e-15,
        "fire danger changed between persistence timestep fixtures"
    );
    const double daily_persistence=std::clamp(
        0.10+0.70*full_day[2],
        0.0,
        0.80
    );
    near(
        full_day[1]/full_day[0],
        daily_persistence,
        1.0e-12,
        "one-day fire persistence changed its calibration"
    );
    near(
        half_day[1]/half_day[0],
        std::sqrt(daily_persistence),
        1.0e-12,
        "fire persistence did not scale with elapsed time"
    );
}

void fuelless_fire_extinguishes() {
    auto simulation=make_default_simulation(
        7002,SimulationConfig{1,1,3'600.0}
    );
    clear_and_dry(*simulation);
    const CellId cell=*simulation->world().active_cells().begin();
    auto& fields=simulation->world().stores().get<FieldStore>();
    ignite(*simulation,cell,0.50);

    run_fire(*simulation);
    near(
        fields.get(cell,field(*simulation,"ecology.fire_danger")),
        0.0,0.0,"fuelless cell retained fire danger"
    );
    near(
        fields.get(
            cell,field(*simulation,"ecology.fire_burned_fraction")
        ),
        0.0,0.0,"fuelless cell burned"
    );
    near(
        fields.get(
            cell,field(*simulation,"ecology.fire_active_fraction")
        ),
        0.0,0.0,"fuelless active fire did not extinguish"
    );
}

void wet_weather_suppresses_fire() {
    auto dry=make_default_simulation(
        7006,SimulationConfig{1,1,3'600.0}
    );
    auto wet=make_default_simulation(
        7006,SimulationConfig{1,1,3'600.0}
    );
    clear_and_dry(*dry);
    clear_and_dry(*wet);
    const CellId cell=*dry->world().active_cells().begin();
    set_fuel(*dry,cell);
    set_fuel(*wet,cell);
    ignite(*dry,cell,0.08);
    ignite(*wet,cell,0.08);

    auto& wet_fields=wet->world().stores().get<FieldStore>();
    const double wet_land_area=
        wet->world().topology().area_m2(cell)*
        wet_fields.get(cell,field(*wet,"geography.land_fraction"));
    wet_fields.set(
        cell,
        field(*wet,"hydrology.soil_water_m3"),
        0.65*soil_water_capacity_depth_m(1.0)*wet_land_area
    );
    wet_fields.set(
        cell,field(*wet,"climate.relative_humidity"),0.95
    );
    wet_fields.set(
        cell,field(*wet,"climate.precipitation_mm_day"),20.0
    );

    run_fire(*dry);
    run_fire(*wet);
    const double dry_burned=dry->world().stores().get<FieldStore>().get(
        cell,field(*dry,"ecology.fire_burned_fraction")
    );
    const double wet_burned=wet_fields.get(
        cell,field(*wet,"ecology.fire_burned_fraction")
    );
    check(
        dry_burned>0.0 && wet_burned<dry_burned*0.01,
        "wet root zone/humid weather did not suppress active fire"
    );
}

void snow_cover_suppresses_fire() {
    auto dry=make_default_simulation(
        7007,SimulationConfig{1,1,3'600.0}
    );
    auto snowy=make_default_simulation(
        7007,SimulationConfig{1,1,3'600.0}
    );
    clear_and_dry(*dry);
    clear_and_dry(*snowy);
    const CellId cell=*dry->world().active_cells().begin();
    set_fuel(*dry,cell);
    set_fuel(*snowy,cell);
    ignite(*dry,cell,0.08);
    ignite(*snowy,cell,0.08);
    snowy->world().stores().get<FieldStore>().set(
        cell,
        field(*snowy,"climate.snow_cover_fraction"),
        1.0
    );

    run_fire(*dry);
    run_fire(*snowy);
    const auto& dry_fields=dry->world().stores().get<FieldStore>();
    const auto& snowy_fields=snowy->world().stores().get<FieldStore>();
    check(
        dry_fields.get(
            cell,field(*dry,"ecology.fire_burned_fraction")
        )>0.0,
        "snow-control fire did not burn"
    );
    near(
        snowy_fields.get(
            cell,field(*snowy,"ecology.fire_danger")
        ),
        0.0,
        0.0,
        "complete snow cover retained fire danger"
    );
}

void spread_resolves_refined_neighbor_region() {
    auto simulation=make_default_simulation(
        7003,SimulationConfig{1,2,3'600.0}
    );
    const CellId source=CellId::make(0,1,0,0);
    const CellId target_region=
        simulation->world().topology().neighbors4(source)[1];
    check(
        target_region==CellId::make(0,1,1,0),
        "fire spread fixture did not select an in-face neighbor"
    );
    simulation->world().refine(target_region);
    clear_and_dry(*simulation);
    for (CellId cell:simulation->world().active_cells()) set_fuel(*simulation,cell);

    auto& fields=simulation->world().stores().get<FieldStore>();
    ignite(*simulation,source,0.10);
    const double source_before=fire_accounted_carbon(*simulation,source);
    run_fire(*simulation);
    near(
        source_before,
        fire_accounted_carbon(*simulation,source),
        2.0e-15,
        "spread source did not close its fire carbon transfers"
    );

    double target_active_area=0.0;
    std::size_t active_children=0;
    for (CellId child:target_region.children()) {
        check(
            simulation->world().active_cells().contains(child),
            "fire spread target is not an active refined child"
        );
        const double active=fields.get(
            child,field(*simulation,"ecology.fire_active_fraction")
        );
        if (active>0.0) ++active_children;
        target_active_area+=
            active*simulation->world().topology().area_m2(child);
    }
    check(
        active_children==4U && target_active_area>0.0,
        "fire did not spread across the refined active neighbor region"
    );

    const double active_area_before= [&] {
        double sum=0.0;
        for (CellId child:target_region.children())
            sum+=fields.get(
                child,
                field(*simulation,"ecology.fire_active_area_m2")
            );
        return sum;
    }();
    const std::array<double,4> land_fraction{0.25,0.50,0.75,1.0};
    for (std::size_t i=0;i<target_region.children().size();++i) {
        fields.set(
            target_region.children()[i],
            field(*simulation,"geography.land_fraction"),
            land_fraction[i]
        );
    }
    check(
        simulation->world().coarsen(target_region),
        "fire LOD fixture could not coarsen target"
    );
    near(
        fields.get(
            target_region,
            field(*simulation,"ecology.fire_active_area_m2")
        ),
        active_area_before,
        1.0e-15,
        "active fire area changed on heterogeneous-land coarsening"
    );
    EcologyModule().on_spatial_cover_changed(
        simulation->world(),simulation->fields()
    );
    const double parent_land_area=
        simulation->world().topology().area_m2(target_region)*
        fields.get(
            target_region,
            field(*simulation,"geography.land_fraction")
        );
    near(
        fields.get(
            target_region,
            field(*simulation,"ecology.fire_active_fraction")
        ),
        active_area_before/parent_land_area,
        1.0e-15,
        "derived active fire fraction ignored conserved active area"
    );

    const double source_emissions=fields.get(
        source,field(*simulation,"ecology.fire_emitted_carbon_kg")
    );
    simulation->world().refine(source);
    double refined_emissions=0.0;
    for (CellId child:source.children())
        refined_emissions+=fields.get(
            child,field(*simulation,"ecology.fire_emitted_carbon_kg")
        );
    near(
        refined_emissions,
        source_emissions,
        1.0e-15,
        "fire carbon ledger changed on refinement"
    );
    check(
        simulation->world().coarsen(source),
        "fire carbon-ledger fixture could not coarsen source"
    );
    near(
        fields.get(
            source,field(*simulation,"ecology.fire_emitted_carbon_kg")
        ),
        source_emissions,
        1.0e-15,
        "fire carbon ledger changed on coarsening"
    );
}

void natural_ignition_is_stateless_and_deterministic() {
    auto first=make_default_simulation(
        7005,SimulationConfig{1,1,3'600.0}
    );
    auto second=make_default_simulation(
        7005,SimulationConfig{1,1,3'600.0}
    );
    for (Simulation* simulation:{first.get(),second.get()}) {
        clear_and_dry(*simulation);
        for (CellId cell:simulation->world().active_cells())
            set_fuel(*simulation,cell);
    }

    bool ignited=false;
    for (Tick day=0;day<128U && !ignited;++day) {
        first->world().set_tick(day*24U+23U);
        second->world().set_tick(day*24U+23U);
        run_fire(*first);
        run_fire(*second);
        check(
            first->save_snapshot()==second->save_snapshot(),
            "same seed/fire state did not reproduce natural ignition"
        );
        const auto first_events=first->world().drain_events();
        const auto second_events=second->world().drain_events();
        check(
            first_events.size()==second_events.size(),
            "deterministic fire copies emitted different event counts"
        );
        if (!first_events.empty()) {
            check(
                first_events.front().type=="ecology.fire_ignited" &&
                first_events.front().cell==second_events.front().cell &&
                first_events.front().magnitude==
                    second_events.front().magnitude,
                "natural fire ignition event is not deterministic"
            );
            ignited=true;
        }
    }
    check(ignited,"bounded dry/fueled run produced no natural ignition");
}

void snapshot_continuation_includes_fire_state() {
    auto simulation=make_default_simulation(
        7004,SimulationConfig{1,2,3'600.0}
    );
    clear_and_dry(*simulation);
    const CellId cell=*simulation->world().active_cells().begin();
    set_fuel(*simulation,cell);
    ignite(*simulation,cell,0.05);
    run_fire(*simulation);

    const auto snapshot=simulation->save_snapshot();
    check(snapshot.size()>11U,"fire snapshot header is unexpectedly short");
    check(snapshot[8]==std::byte{33},"unexpected current snapshot epoch");
    auto restored=make_default_simulation(
        7004,SimulationConfig{1,2,3'600.0}
    );
    restored->load_snapshot(snapshot);
    check(
        restored->save_snapshot()==snapshot,
        "wildfire snapshot roundtrip changed authoritative state"
    );
}

} // namespace

int main() {
    try {
        controlled_fire_closes_carbon();
        burn_cap_scales_with_elapsed_time();
        persistence_scales_with_elapsed_time();
        fuelless_fire_extinguishes();
        wet_weather_suppresses_fire();
        snow_cover_suppresses_fire();
        spread_resolves_refined_neighbor_region();
        natural_ignition_is_stateless_and_deterministic();
        snapshot_continuation_includes_fire_state();
        std::cout<<"fire_tests: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr<<"fire_tests: FAIL: "<<error.what()<<'\n';
        return EXIT_FAILURE;
    }
}

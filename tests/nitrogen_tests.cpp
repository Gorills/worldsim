#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"
#include "worldsim/planetary_nitrogen.hpp"
#include "worldsim/soil_carbon.hpp"
#include "worldsim/soil_nitrogen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

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

FieldId field(const Simulation& simulation, std::string_view key) {
    const auto id=simulation.fields().find(key);
    if (!id) throw std::runtime_error("missing nitrogen fixture field");
    return *id;
}

CellId land_cell(Simulation& simulation) {
    const auto land=field(simulation,"geography.land_fraction");
    const auto& fields=simulation.world().stores().get<FieldStore>();
    for (CellId cell:simulation.world().active_cells()) {
        if (fields.get(cell,land)>0.10) return cell;
    }
    throw std::runtime_error("nitrogen fixture found no land cell");
}

void run_system(
    Simulation& simulation,
    std::string_view id,
    double dt_days
) {
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
        if (system->id()==id) {
            system->step(context);
            return;
        }
    }
    throw std::runtime_error("nitrogen fixture system is absent");
}

double sum_field(const Simulation& simulation, std::string_view key) {
    const FieldId id=field(simulation,key);
    const auto& fields=simulation.world().stores().get<FieldStore>();
    double total=0.0;
    for (double value:fields.column(id)) total+=value;
    return total;
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

void soil_system_closes_and_reports_fluxes() {
    auto simulation=make_default_simulation(
        8201,SimulationConfig{1,1,3'600.0}
    );
    const CellId cell=land_cell(*simulation);
    auto& fields=simulation->world().stores().get<FieldStore>();
    const double area=
        simulation->world().topology().area_m2(cell)*
        fields.get(cell,field(*simulation,"geography.land_fraction"));

    fields.set(
        cell,field(*simulation,"geology.regolith_thickness_m"),1.0
    );
    fields.set(
        cell,field(*simulation,"climate.surface_temperature_k"),293.0
    );
    fields.set(
        cell,field(*simulation,"hydrology.soil_water_m3"),
        0.60*soil_water_capacity_depth_m(1.0)*area
    );
    fields.set(
        cell,field(*simulation,"hydrology.drainage_since_soil_m3"),
        0.02*area
    );
    fields.set(
        cell,field(*simulation,"ecology.litter_carbon_kg"),1'000.0
    );
    fields.set(
        cell,field(*simulation,"ecology.soil_fast_carbon_kg"),500.0
    );
    fields.set(
        cell,field(*simulation,"ecology.soil_slow_carbon_kg"),5'000.0
    );
    fields.set(
        cell,field(*simulation,"ecology.litter_nitrogen_kg"),25.0
    );
    fields.set(
        cell,field(*simulation,"ecology.soil_fast_nitrogen_kg"),35.0
    );
    fields.set(
        cell,field(*simulation,"ecology.soil_slow_nitrogen_kg"),400.0
    );
    fields.set(
        cell,field(*simulation,"ecology.mineral_nitrogen_kg"),2.0
    );

    const double before=total_ecology_nitrogen_accounted_kg(
        simulation->world(),simulation->fields()
    );
    const double leached_before=fields.get(
        cell,field(*simulation,"ecology.nitrogen_leached_kg")
    );
    run_system(*simulation,"ecology.soil",1.0);
    const double after=total_ecology_nitrogen_accounted_kg(
        simulation->world(),simulation->fields()
    );
    near(before,after,3.0e-15,"soil system lost tracked nitrogen");
    check(
        fields.get(
            cell,
            field(*simulation,"ecology.nitrogen_mineralization_kg_day")
        )>0.0,
        "soil system reported no nitrogen mineralization"
    );
    check(
        fields.get(
            cell,field(*simulation,"ecology.nitrogen_leached_kg")
        )>leached_before,
        "soil drainage did not enter the nitrogen leaching ledger"
    );
    near(
        fields.get(
            cell,field(*simulation,"ecology.nitrogen_leaching_kg_day")
        ),
        fields.get(
            cell,field(*simulation,"ecology.nitrogen_leached_kg")
        )-leached_before,
        2.0e-15,
        "soil nitrogen leaching rate disagrees with its one-day ledger delta"
    );
}

void vegetation_is_limited_by_finite_nitrogen() {
    auto make_fixture=[](double mineral_density) {
        auto simulation=make_default_simulation(
            8202,SimulationConfig{1,1,3'600.0}
        );
        const CellId cell=land_cell(*simulation);
        auto& fields=simulation->world().stores().get<FieldStore>();
        const double area=
            simulation->world().topology().area_m2(cell)*
            fields.get(cell,field(*simulation,"geography.land_fraction"));
        const double grass_carbon=0.50*area;

        for (std::string_view key:{
                 "ecology.shrub_carbon_kg",
                 "ecology.tree_carbon_kg",
                 "ecology.shrub_nitrogen_kg",
                 "ecology.tree_nitrogen_kg"
             }) {
            fields.set(cell,field(*simulation,key),0.0);
        }
        fields.set(
            cell,field(*simulation,"ecology.grass_carbon_kg"),
            grass_carbon
        );
        fields.set(
            cell,field(*simulation,"ecology.vegetation_carbon_kg"),
            grass_carbon
        );
        fields.set(
            cell,field(*simulation,"ecology.grass_nitrogen_kg"),
            grass_carbon/kPlantCarbonNitrogenRatio[0]
        );
        fields.set(
            cell,field(*simulation,"ecology.vegetation_nitrogen_kg"),
            grass_carbon/kPlantCarbonNitrogenRatio[0]
        );
        fields.set(
            cell,field(*simulation,"ecology.mineral_nitrogen_kg"),
            mineral_density*area
        );
        // Hold the rate modifier equal; this isolates the finite-stock cap.
        fields.set(
            cell,field(*simulation,"ecology.soil_fertility"),1.0
        );
        fields.set(
            cell,field(*simulation,"geology.regolith_thickness_m"),1.0
        );
        fields.set(
            cell,field(*simulation,"climate.surface_temperature_k"),286.0
        );
        fields.set(
            cell,field(*simulation,"climate.solar_flux_w_m2"),340.0
        );
        fields.set(
            cell,field(*simulation,"climate.snow_cover_fraction"),0.0
        );
        fields.set(
            cell,field(*simulation,"hydrology.flooded_fraction"),0.0
        );
        fields.set(
            cell,field(*simulation,"hydrology.inundation_days"),0.0
        );
        fields.set(
            cell,field(*simulation,"hydrology.soil_water_m3"),
            0.60*soil_water_capacity_depth_m(1.0)*area
        );
        return std::pair{std::move(simulation),cell};
    };

    auto [poor,poor_cell]=make_fixture(0.0);
    auto [rich,rich_cell]=make_fixture(0.02);
    const double poor_before=total_ecology_nitrogen_accounted_kg(
        poor->world(),poor->fields()
    );
    const double rich_before=total_ecology_nitrogen_accounted_kg(
        rich->world(),rich->fields()
    );
    run_system(*poor,"ecology.vegetation",1.0);
    run_system(*rich,"ecology.vegetation",1.0);

    const auto& poor_fields=poor->world().stores().get<FieldStore>();
    const auto& rich_fields=rich->world().stores().get<FieldStore>();
    check(
        rich_fields.get(
            rich_cell,field(*rich,"ecology.npp_kg_day")
        )>
        poor_fields.get(
            poor_cell,field(*poor,"ecology.npp_kg_day")
        ),
        "finite mineral nitrogen did not limit vegetation production"
    );
    near(
        poor_fields.get(
            poor_cell,field(*poor,"ecology.nitrogen_uptake_kg_day")
        ),
        0.0,0.0,
        "nitrogen-free vegetation reported nitrogen uptake"
    );
    check(
        rich_fields.get(
            rich_cell,field(*rich,"ecology.nitrogen_uptake_kg_day")
        )>0.0,
        "nitrogen-rich vegetation reported no nitrogen uptake"
    );
    near(
        poor_before,
        total_ecology_nitrogen_accounted_kg(
            poor->world(),poor->fields()
        ),
        3.0e-15,
        "nitrogen-limited vegetation did not conserve nitrogen"
    );
    near(
        rich_before,
        total_ecology_nitrogen_accounted_kg(
            rich->world(),rich->fields()
        ),
        3.0e-15,
        "nitrogen-fed vegetation did not conserve nitrogen"
    );
}

void planetary_reservoirs_close_boundary_and_return_fluxes() {
    auto simulation=make_default_simulation(
        8203,SimulationConfig{1,1,3'600.0}
    );
    const CellId cell=land_cell(*simulation);
    auto& fields=simulation->world().stores().get<FieldStore>();
    auto& store=simulation->world().stores().get<NitrogenStore>();

    const FieldId mineral=field(
        *simulation,"ecology.mineral_nitrogen_kg"
    );
    const FieldId litter=field(
        *simulation,"ecology.litter_nitrogen_kg"
    );
    const FieldId fertility=field(
        *simulation,"ecology.soil_fertility"
    );
    const double leached=std::min(10.0,0.10*fields.get(cell,mineral));
    const double fire_loss=std::min(5.0,0.10*fields.get(cell,litter));
    check(
        leached>0.0 && fire_loss>0.0,
        "planetary nitrogen fixture lacks terrestrial donor stocks"
    );

    const double before=total_planet_nitrogen_kg(
        simulation->world(),simulation->fields()
    );
    const double leaching_budget_before=
        store.budget().terrestrial_leached_to_ocean_kg;
    fields.add(cell,mineral,-leached);
    fields.add(cell,litter,-fire_loss);
    fields.set(cell,fertility,0.0);
    store.advance(
        simulation->world(),
        simulation->fields(),
        leached,
        fire_loss,
        1.0
    );

    near(
        before,
        total_planet_nitrogen_kg(
            simulation->world(),simulation->fields()
        ),
        3.0e-15,
        "planetary nitrogen reservoirs did not close boundary transfers"
    );
    near(
        store.budget().terrestrial_leached_to_ocean_kg-
            leaching_budget_before,
        leached,
        2.0e-15,
        "terrestrial leaching did not enter the ocean nitrogen reservoir"
    );
    check(
        store.atmospheric_reactive_nitrogen_kg()>0.0,
        "fire nitrogen did not enter the reactive atmospheric reservoir"
    );
    check(
        store.budget().fixed_from_atmosphere_kg>0.0,
        "nitrogen scarcity did not trigger atmospheric fixation"
    );
    check(
        store.budget().reactive_deposited_kg>0.0,
        "reactive atmospheric nitrogen did not redeposit to land"
    );
    check(
        sum_field(*simulation,"ecology.nitrogen_fixation_kg_day")>0.0,
        "planetary nitrogen system did not expose fixation flux"
    );
    check(
        sum_field(*simulation,"ecology.nitrogen_deposition_kg_day")>0.0,
        "planetary nitrogen system did not expose deposition flux"
    );
}

void fixation_uses_authoritative_mineral_stock() {
    auto make_fixture=[](double mineral_density) {
        auto simulation=make_default_simulation(
            8206,SimulationConfig{1,1,3'600.0}
        );
        const CellId cell=land_cell(*simulation);
        auto& fields=simulation->world().stores().get<FieldStore>();
        const double area=
            simulation->world().topology().area_m2(cell)*
            fields.get(
                cell,field(*simulation,"geography.land_fraction")
            );
        fields.set(
            cell,field(*simulation,"ecology.mineral_nitrogen_kg"),
            mineral_density*area
        );
        // Deliberately stale diagnostic: the planetary cycle must use the
        // authoritative mineral stock rather than trusting this value.
        fields.set(
            cell,field(*simulation,"ecology.soil_fertility"),0.0
        );
        run_system(*simulation,"ecology.nitrogen_cycle",1.0);
        return std::pair{
            std::move(simulation),
            cell
        };
    };

    auto [poor,poor_cell]=make_fixture(0.0);
    auto [rich,rich_cell]=make_fixture(0.03);
    const auto& poor_fields=poor->world().stores().get<FieldStore>();
    const auto& rich_fields=rich->world().stores().get<FieldStore>();
    const double poor_fixation=poor_fields.get(
        poor_cell,field(*poor,"ecology.nitrogen_fixation_kg_day")
    );
    const double rich_fixation=rich_fields.get(
        rich_cell,field(*rich,"ecology.nitrogen_fixation_kg_day")
    );
    check(
        poor_fixation>rich_fixation,
        "nitrogen fixation trusted stale fertility instead of mineral stock"
    );
}

void coupled_scheduler_closes_planetary_nitrogen() {
    auto simulation=make_default_simulation(
        8205,SimulationConfig{1,1,3'600.0}
    );
    const double before=total_planet_nitrogen_kg(
        simulation->world(),simulation->fields()
    );
    simulation->step(24U*30U);
    const double after=total_planet_nitrogen_kg(
        simulation->world(),simulation->fields()
    );
    near(
        before,after,2.0e-12,
        "coupled daily scheduler drifted planetary nitrogen"
    );
    check(
        sum_field(*simulation,"ecology.nitrogen_uptake_kg_day")>0.0,
        "coupled ecology reported no plant nitrogen uptake"
    );
    check(
        sum_field(
            *simulation,"ecology.nitrogen_mineralization_kg_day"
        )>0.0,
        "coupled ecology reported no nitrogen mineralization"
    );
    check(
        simulation->world().stores().get<NitrogenStore>()
            .budget().fixed_from_atmosphere_kg>0.0,
        "coupled ecology performed no atmospheric nitrogen fixation"
    );
}

void lod_and_snapshot_preserve_nitrogen() {
    auto simulation=make_default_simulation(
        8204,SimulationConfig{1,2,3'600.0}
    );
    const CellId parent=CellId::make(0,1,0,0);
    auto& fields=simulation->world().stores().get<FieldStore>();
    const std::array<std::pair<std::string_view,double>,5> stocks{{
        {"ecology.mineral_nitrogen_kg",17.0},
        {"ecology.litter_nitrogen_kg",23.0},
        {"ecology.soil_fast_nitrogen_kg",41.0},
        {"ecology.soil_slow_nitrogen_kg",83.0},
        {"ecology.grass_nitrogen_kg",13.0}
    }};
    for (const auto& [key,value]:stocks)
        fields.set(parent,field(*simulation,key),value);

    const double before=total_ecology_nitrogen_accounted_kg(
        simulation->world(),simulation->fields()
    );
    simulation->world().refine(parent);
    check(
        simulation->world().coarsen(parent),
        "nitrogen LOD fixture could not coarsen"
    );
    EcologyModule().on_spatial_cover_changed(
        simulation->world(),simulation->fields()
    );
    near(
        before,
        total_ecology_nitrogen_accounted_kg(
            simulation->world(),simulation->fields()
        ),
        2.0e-15,
        "nitrogen changed across refine/coarsen"
    );
    near(
        fields.get(
            parent,field(*simulation,"ecology.vegetation_nitrogen_kg")
        ),
        fields.get(
            parent,field(*simulation,"ecology.grass_nitrogen_kg")
        )+
        fields.get(
            parent,field(*simulation,"ecology.shrub_nitrogen_kg")
        )+
        fields.get(
            parent,field(*simulation,"ecology.tree_nitrogen_kg")
        ),
        2.0e-15,
        "vegetation nitrogen aggregate did not reconstruct after LOD"
    );

    simulation->step(48);
    const auto snapshot=simulation->save_snapshot();
    check(
        snapshot.size()>11U && snapshot[8]==std::byte{37},
        "unexpected nitrogen-cycle snapshot epoch"
    );
    auto restored=make_default_simulation(
        8204,SimulationConfig{1,2,3'600.0}
    );
    restored->load_snapshot(snapshot);
    check(
        restored->save_snapshot()==snapshot,
        "nitrogen snapshot roundtrip changed authoritative state"
    );
    simulation->step(48);
    restored->step(48);
    check(
        restored->save_snapshot()==simulation->save_snapshot(),
        "nitrogen snapshot continuation diverged"
    );
}

} // namespace

int main() {
    try {
        soil_nitrogen_closes_against_carbon_fluxes();
        new_organic_transfer_is_staged();
        fertility_tracks_finite_mineral_stock();
        soil_system_closes_and_reports_fluxes();
        vegetation_is_limited_by_finite_nitrogen();
        planetary_reservoirs_close_boundary_and_return_fluxes();
        fixation_uses_authoritative_mineral_stock();
        coupled_scheduler_closes_planetary_nitrogen();
        lod_and_snapshot_preserve_nitrogen();
        std::cout<<"nitrogen_tests: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr<<"nitrogen_tests: FAIL: "<<error.what()<<'\n';
        return EXIT_FAILURE;
    }
}

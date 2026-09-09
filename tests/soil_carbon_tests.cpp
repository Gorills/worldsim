#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"
#include "worldsim/soil_carbon.hpp"

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
    if (!std::isfinite(a) || !std::isfinite(b) ||
        std::abs(a-b)>relative*scale) {
        throw std::runtime_error(message);
    }
}

template<class F>
void rejects(F&& operation, const char* message) {
    bool rejected=false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected=true;
    }
    check(rejected,message);
}

FieldId field(const Simulation& simulation, std::string_view key) {
    const auto id=simulation.fields().find(key);
    if (!id) throw std::runtime_error("missing soil-carbon fixture field");
    return *id;
}

void run_soil(Simulation& simulation, double dt_days) {
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
        if (system->id()=="ecology.soil") {
            system->step(context);
            return;
        }
    }
    throw std::runtime_error("soil system is absent");
}

double soil_accounted_carbon(
    const Simulation& simulation,
    CellId cell
) {
    const auto& fields=simulation.world().stores().get<FieldStore>();
    double total=0.0;
    for (std::string_view key:{
             "ecology.litter_carbon_kg",
             "ecology.soil_fast_carbon_kg",
             "ecology.soil_slow_carbon_kg",
             "ecology.soil_respired_carbon_kg"
         }) {
        total+=fields.get(cell,field(simulation,key));
    }
    return total;
}

void model_closes_and_stages_transfers() {
    const SoilCarbonModel model;
    const SoilCarbonState initial{100.0,40.0,1'000.0};
    const SoilCarbonStep result=model.advance(
        initial,293.0,0.70,30.0
    );
    near(
        initial.litter_carbon_kg+
            initial.fast_carbon_kg+
            initial.slow_carbon_kg,
        result.state.litter_carbon_kg+
            result.state.fast_carbon_kg+
            result.state.slow_carbon_kg+
            result.fluxes.respired_kg,
        2.0e-15,
        "soil carbon model does not close"
    );

    const SoilCarbonStep exhausted=model.advance(
        {100.0,0.0,0.0},293.0,1.0,1.0e9
    );
    near(
        exhausted.state.fast_carbon_kg,
        55.0,
        2.0e-15,
        "new litter transfer decomposed again in the same step"
    );
    near(
        exhausted.state.slow_carbon_kg,
        0.0,
        0.0,
        "new carbon crossed multiple pools in one step"
    );
}

void environment_controls_decomposition() {
    const SoilCarbonModel model;
    const SoilCarbonState initial{100.0,100.0,100.0};
    const auto cold_dry=model.advance(initial,263.0,0.0,30.0);
    const auto warm_wet=model.advance(initial,303.0,1.0,30.0);
    check(
        warm_wet.fluxes.respired_kg>
            cold_dry.fluxes.respired_kg*20.0,
        "warm moist soil did not accelerate carbon turnover"
    );
    check(
        warm_wet.state.litter_carbon_kg<
            cold_dry.state.litter_carbon_kg,
        "environmental response did not affect litter"
    );
}

void model_rejects_invalid_inputs() {
    const SoilCarbonModel model;
    rejects(
        [&]{ (void)model.advance({-1.0,0.0,0.0},283.0,0.5,1.0); },
        "soil carbon accepted a negative stock"
    );
    rejects(
        [&]{
            (void)model.advance(
                {1.0,0.0,0.0},
                std::numeric_limits<double>::infinity(),
                0.5,
                1.0
            );
        },
        "soil carbon accepted a non-finite temperature"
    );
    rejects(
        [&]{
            (void)model.advance(
                {1.0,0.0,0.0},
                283.0,
                std::numeric_limits<double>::quiet_NaN(),
                1.0
            );
        },
        "soil carbon accepted non-finite moisture"
    );
    rejects(
        [&]{ (void)model.advance({1.0,0.0,0.0},283.0,0.5,-1.0); },
        "soil carbon accepted a negative timestep"
    );
}

void system_closes_short_and_long_steps() {
    for (double dt_days:{0.25,10'000.0}) {
        auto simulation=make_default_simulation(
            8101,SimulationConfig{1,1,3'600.0}
        );
        const CellId cell=*simulation->world().active_cells().begin();
        auto& fields=simulation->world().stores().get<FieldStore>();
        const double area=simulation->world().topology().area_m2(cell);
        fields.set(cell,field(*simulation,"geography.land_fraction"),1.0);
        fields.set(
            cell,field(*simulation,"geology.regolith_thickness_m"),1.0
        );
        fields.set(
            cell,field(*simulation,"climate.surface_temperature_k"),293.0
        );
        fields.set(
            cell,
            field(*simulation,"hydrology.soil_water_m3"),
            0.60*soil_water_capacity_depth_m(1.0)*area
        );
        fields.set(
            cell,field(*simulation,"hydrology.drainage_since_soil_m3"),0.0
        );
        fields.set(cell,field(*simulation,"ecology.litter_carbon_kg"),1'000.0);
        fields.set(cell,field(*simulation,"ecology.soil_fast_carbon_kg"),500.0);
        fields.set(cell,field(*simulation,"ecology.soil_slow_carbon_kg"),5'000.0);
        fields.set(cell,field(*simulation,"ecology.soil_respired_carbon_kg"),17.0);

        const double before=soil_accounted_carbon(*simulation,cell);
        const double ledger_before=fields.get(
            cell,field(*simulation,"ecology.soil_respired_carbon_kg")
        );
        run_soil(*simulation,dt_days);
        const double ledger_after=fields.get(
            cell,field(*simulation,"ecology.soil_respired_carbon_kg")
        );
        near(
            before,
            soil_accounted_carbon(*simulation,cell),
            2.0e-15,
            "soil system lost carbon"
        );
        near(
            fields.get(
                cell,
                field(*simulation,"ecology.heterotrophic_respiration_kg_day")
            ),
            (ledger_after-ledger_before)/dt_days,
            1.0e-12,
            "respiration flux disagrees with cumulative ledger"
        );
        near(
            fields.get(cell,field(*simulation,"ecology.soil_carbon_kg")),
            fields.get(
                cell,field(*simulation,"ecology.soil_fast_carbon_kg")
            )+
            fields.get(
                cell,field(*simulation,"ecology.soil_slow_carbon_kg")
            ),
            1.0e-15,
            "soil aggregate disagrees with component pools"
        );
    }
}

void submerged_stock_is_dormant() {
    auto simulation=make_default_simulation(
        8102,SimulationConfig{1,1,3'600.0}
    );
    const CellId cell=*simulation->world().active_cells().begin();
    auto& fields=simulation->world().stores().get<FieldStore>();
    fields.set(cell,field(*simulation,"geography.land_fraction"),0.0);
    fields.set(cell,field(*simulation,"ecology.litter_carbon_kg"),10.0);
    fields.set(cell,field(*simulation,"ecology.soil_fast_carbon_kg"),20.0);
    fields.set(cell,field(*simulation,"ecology.soil_slow_carbon_kg"),30.0);
    fields.set(cell,field(*simulation,"ecology.soil_respired_carbon_kg"),40.0);
    const double before=soil_accounted_carbon(*simulation,cell);

    run_soil(*simulation,365.0);
    near(
        before,soil_accounted_carbon(*simulation,cell),0.0,
        "submerged soil carbon was silently removed"
    );
    near(
        fields.get(
            cell,
            field(*simulation,"ecology.heterotrophic_respiration_kg_day")
        ),
        0.0,0.0,
        "submerged soil retained a decomposition flux"
    );
    near(
        fields.get(cell,field(*simulation,"ecology.soil_carbon_kg")),
        50.0,0.0,
        "submerged soil aggregate was not reconstructed"
    );
}

void lod_preserves_components_and_ledger() {
    auto simulation=make_default_simulation(
        8103,SimulationConfig{1,2,3'600.0}
    );
    const CellId parent=CellId::make(0,1,0,0);
    auto& fields=simulation->world().stores().get<FieldStore>();
    for (const auto& [key,value]:{
             std::pair<std::string_view,double>{
                 "ecology.soil_fast_carbon_kg",101.0
             },
             {"ecology.soil_slow_carbon_kg",503.0},
             {"ecology.soil_carbon_kg",604.0},
             {"ecology.soil_respired_carbon_kg",79.0}
         }) {
        fields.set(parent,field(*simulation,key),value);
    }

    simulation->world().refine(parent);
    for (const auto& [key,expected]:{
             std::pair<std::string_view,double>{
                 "ecology.soil_fast_carbon_kg",101.0
             },
             {"ecology.soil_slow_carbon_kg",503.0},
             {"ecology.soil_respired_carbon_kg",79.0}
         }) {
        double sum=0.0;
        for (CellId child:parent.children())
            sum+=fields.get(child,field(*simulation,key));
        near(sum,expected,2.0e-15,"soil field changed on refinement");
    }
    check(
        simulation->world().coarsen(parent),
        "soil-carbon LOD fixture could not coarsen"
    );
    EcologyModule().on_spatial_cover_changed(
        simulation->world(),simulation->fields()
    );
    near(
        fields.get(parent,field(*simulation,"ecology.soil_fast_carbon_kg")),
        101.0,2.0e-15,"fast soil carbon changed on coarsening"
    );
    near(
        fields.get(parent,field(*simulation,"ecology.soil_slow_carbon_kg")),
        503.0,2.0e-15,"slow soil carbon changed on coarsening"
    );
    near(
        fields.get(parent,field(*simulation,"ecology.soil_carbon_kg")),
        604.0,2.0e-15,"soil aggregate changed on coarsening"
    );
    near(
        fields.get(
            parent,field(*simulation,"ecology.soil_respired_carbon_kg")
        ),
        79.0,2.0e-15,"soil respiration ledger changed on coarsening"
    );
}

void snapshot_epoch_and_continuation() {
    auto simulation=make_default_simulation(
        8104,SimulationConfig{1,2,3'600.0}
    );
    simulation->step(24U*5U);
    const auto snapshot=simulation->save_snapshot();
    check(snapshot.size()>11U,"soil-carbon snapshot header is too short");
    check(
        snapshot[8]==std::byte{34},
        "unexpected current snapshot epoch"
    );
    auto restored=make_default_simulation(
        8104,SimulationConfig{1,2,3'600.0}
    );
    restored->load_snapshot(snapshot);
    check(
        restored->save_snapshot()==snapshot,
        "soil-carbon snapshot roundtrip changed state"
    );
    simulation->step(48);
    restored->step(48);
    check(
        restored->save_snapshot()==simulation->save_snapshot(),
        "soil-carbon snapshot lost deterministic continuation"
    );
}

} // namespace

int main() {
    try {
        model_closes_and_stages_transfers();
        environment_controls_decomposition();
        model_rejects_invalid_inputs();
        system_closes_short_and_long_steps();
        submerged_stock_is_dormant();
        lod_preserves_components_and_ledger();
        snapshot_epoch_and_continuation();
        std::cout<<"soil_carbon_tests: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr<<"soil_carbon_tests: FAIL: "<<error.what()<<'\n';
        return EXIT_FAILURE;
    }
}

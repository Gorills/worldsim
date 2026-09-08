#include "worldsim/climate.hpp"
#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

using namespace worldsim;
namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(double a, double b, double relative, const char* message) {
    check(
        std::isfinite(a) && std::isfinite(b) &&
        std::abs(a-b)<=relative*std::max({1.0,std::abs(a),std::abs(b)}),
        message
    );
}

class SurfaceFixtureModule final : public ISimModule {
public:
    explicit SurfaceFixtureModule(bool mountain=false):mountain_(mountain) {}
    std::string_view id() const override { return "fixture.surface"; }
    void register_fields(FieldRegistry& r) override {
        r.register_field({
            "geography.elevation_m","m",
            FieldSemantics::Intensive,0.0,-11'000.0,9'000.0
        });
        r.register_field({
            "geography.land_fraction","1",
            FieldSemantics::Intensive,0.5,0.0,1.0
        });
    }
    void initialize(WorldState& world, const FieldRegistry& r) override {
        auto& fields=world.stores().get<FieldStore>();
        const FieldId elevation=*r.find("geography.elevation_m");
        const FieldId land=*r.find("geography.land_fraction");
        for (CellId cell:world.active_cells()) {
            const auto [latitude,longitude]=world.topology().lat_lon_rad(cell);
            const bool ridge=mountain_ &&
                std::abs(latitude)<25.0*kPi/180.0 &&
                std::abs(longitude)<18.0*kPi/180.0;
            fields.set(cell,elevation,ridge ? 3'000.0 : 0.0);
            fields.set(cell,land,0.5);
        }
    }
private:
    bool mountain_{};
};

std::unique_ptr<Simulation> climate_fixture(
    std::uint64_t seed,
    std::uint8_t base_level,
    std::uint8_t max_level,
    double tick_seconds,
    bool mountain=false
) {
    auto simulation=std::make_unique<Simulation>(
        seed,SimulationConfig{base_level,max_level,tick_seconds}
    );
    simulation->add_module(
        std::make_unique<SurfaceFixtureModule>(mountain)
    );
    simulation->add_module(std::make_unique<ClimateModule>());
    simulation->build();
    return simulation;
}

void optional_magic_contract() {
    auto climate_only=climate_fixture(42,1,1,3'600.0);
    climate_only->step(24);
    check(
        climate_only->world().stores()
            .get<ClimateStore>()
            .total_atmospheric_water_m3()>0.0,
        "climate without magic did not retain atmospheric water"
    );

    auto coupled=std::make_unique<Simulation>(
        42,SimulationConfig{1,1,3'600.0}
    );
    coupled->add_module(std::make_unique<GeographyModule>());
    coupled->add_module(std::make_unique<ClimateModule>());
    coupled->add_module(std::make_unique<HydrologyModule>());
    coupled->add_module(std::make_unique<EcologyModule>());
    coupled->build();
    coupled->step(24);
    check(
        coupled->world().stores().get<HydrologyStore>().nodes().size()==24U,
        "coupled climate/hydrology without magic did not run"
    );
    check(
        coupled->fields().find("ecology.vegetation_carbon_kg").has_value(),
        "ecology without magic did not build"
    );
}

void energy_and_moisture_accounting() {
    auto simulation=climate_fixture(77,1,1,86'400.0);
    auto& store=simulation->world().stores().get<ClimateStore>();
    const double initial_heat=store.total_surface_heat_j();
    const double initial_water=
        store.total_atmospheric_water_m3()+store.ocean_water_m3();
    simulation->step(365);
    const ClimateBudget& budget=store.budget();
    near(
        store.total_surface_heat_j(),
        initial_heat+budget.absorbed_solar_j-budget.outgoing_longwave_j,
        2.0e-12,
        "surface energy ledger does not close"
    );
    near(
        store.total_atmospheric_water_m3()+store.ocean_water_m3(),
        initial_water,
        2.0e-12,
        "climate-only water inventory does not close"
    );
    for (const ClimateNode& node:store.nodes()) {
        check(
            std::isfinite(node.atmospheric_water_m3) &&
            node.atmospheric_water_m3>=0.0 &&
            std::isfinite(node.precipitation_m3_day) &&
            node.precipitation_m3_day>=0.0,
            "climate produced an invalid atmospheric-water state"
        );
    }
}

void seasonal_thermal_inertia() {
    auto simulation=climate_fixture(88,2,2,86'400.0);
    const auto& initial=simulation->world().stores().get<ClimateStore>();
    std::size_t monitor=0;
    double best=std::numeric_limits<double>::infinity();
    for (std::size_t i=0;i<initial.nodes().size();++i) {
        const double latitude=std::abs(
            simulation->world().topology().lat_lon_rad(
                initial.nodes()[i].cell
            ).first*180.0/kPi
        );
        const double difference=std::abs(latitude-45.0);
        if (difference<best) {
            best=difference;
            monitor=i;
        }
    }

    simulation->step(365);
    double land_min=std::numeric_limits<double>::infinity();
    double land_max=-std::numeric_limits<double>::infinity();
    double ocean_min=std::numeric_limits<double>::infinity();
    double ocean_max=-std::numeric_limits<double>::infinity();
    int land_peak=0;
    int ocean_peak=0;
    for (int day=0;day<365;++day) {
        simulation->step();
        const ClimateNode& node=simulation->world().stores()
            .get<ClimateStore>().nodes()[monitor];
        if (node.land_temperature_k>land_max) {
            land_max=node.land_temperature_k;
            land_peak=day;
        }
        if (node.ocean_temperature_k>ocean_max) {
            ocean_max=node.ocean_temperature_k;
            ocean_peak=day;
        }
        land_min=std::min(land_min,node.land_temperature_k);
        ocean_min=std::min(ocean_min,node.ocean_temperature_k);
    }
    const double land_amplitude=land_max-land_min;
    const double ocean_amplitude=ocean_max-ocean_min;
    check(
        land_amplitude>ocean_amplitude*1.5,
        "ocean mixed layer lacks reduced seasonal amplitude"
    );
    const int lag=(ocean_peak-land_peak+365)%365;
    check(
        lag>=3 && lag<=150,
        "ocean mixed layer lacks a bounded seasonal phase lag"
    );
}

void orographic_precipitation() {
    auto simulation=climate_fixture(91,3,3,86'400.0,true);
    simulation->step(5);
    const auto& fields=simulation->world().stores().get<FieldStore>();
    const FieldId precipitation=
        *simulation->fields().find("climate.precipitation_mm_day");
    double ridge_sum=0.0;
    double lee_sum=0.0;
    std::size_t ridge_count=0;
    std::size_t lee_count=0;
    for (CellId cell:simulation->world().active_cells()) {
        const auto [latitude,longitude]=
            simulation->world().topology().lat_lon_rad(cell);
        const double lat=std::abs(latitude*180.0/kPi);
        const double lon=longitude*180.0/kPi;
        if (lat>=25.0) continue;
        if (std::abs(lon)<18.0) {
            ridge_sum+=fields.get(cell,precipitation);
            ++ridge_count;
        } else if (lon>-55.0 && lon<-20.0) {
            lee_sum+=fields.get(cell,precipitation);
            ++lee_count;
        }
    }
    check(ridge_count>0U && lee_count>0U,"orographic fixture has no samples");
    check(
        ridge_sum/static_cast<double>(ridge_count)>
            lee_sum/static_cast<double>(lee_count)*1.05,
        "mountain barrier lacks a windward precipitation enhancement"
    );
}

void lod_independent_reference_state() {
    auto coarse=climate_fixture(101,1,2,3'600.0);
    auto focused=climate_fixture(101,1,2,3'600.0);
    focused->set_focus({1.0,0.2,0.1});
    coarse->step(72);
    focused->step(72);
    const auto& a=coarse->world().stores().get<ClimateStore>();
    const auto& b=focused->world().stores().get<ClimateStore>();
    check(a.nodes().size()==b.nodes().size(),"focus changed climate node count");
    for (std::size_t i=0;i<a.nodes().size();++i) {
        check(a.nodes()[i].cell==b.nodes()[i].cell,"focus changed climate node identity");
        near(a.nodes()[i].land_temperature_k,b.nodes()[i].land_temperature_k,2e-12,
            "focus changed reference land temperature");
        near(a.nodes()[i].ocean_temperature_k,b.nodes()[i].ocean_temperature_k,2e-12,
            "focus changed reference ocean temperature");
        near(a.nodes()[i].atmospheric_water_m3,b.nodes()[i].atmospheric_water_m3,2e-12,
            "focus changed reference atmospheric water");
    }
}

void coupled_planet_water_and_snapshot() {
    auto simulation=make_default_simulation(
        202,SimulationConfig{1,2,3'600.0}
    );
    const auto& initial_store=
        simulation->world().stores().get<ClimateStore>();
    const double active_scale=
        initial_store.total_atmospheric_water_m3()+
        total_land_water_m3(simulation->world(),simulation->fields());
    const double initial=total_planet_water_m3(
        simulation->world(),simulation->fields()
    );
    simulation->set_focus({1.0,0.3,0.2});
    simulation->step(24*45);
    const double final=total_planet_water_m3(
        simulation->world(),simulation->fields()
    );
    check(
        std::abs(final-initial)/std::max(1.0,active_scale)<2.0e-9,
        "coupled planet water inventory does not close"
    );
    const auto snapshot=simulation->save_snapshot();
    auto restored=make_default_simulation(
        202,SimulationConfig{1,2,3'600.0}
    );
    restored->load_snapshot(snapshot);
    check(restored->save_snapshot()==snapshot,"climate snapshot roundtrip differs");
    simulation->step(48);
    restored->step(48);
    check(
        restored->save_snapshot()==simulation->save_snapshot(),
        "climate snapshot continuation diverged"
    );
}

void malformed_climate_store_is_rejected() {
    auto simulation=climate_fixture(303,1,1,3'600.0);
    auto& store=simulation->world().stores().get<ClimateStore>();
    BinaryWriter writer;
    store.save(writer);
    auto bytes=writer.data();
    // Header: reference level, ocean water, seven budget values and count.
    // The sixth node double is atmospheric water.
    constexpr std::size_t first_atmospheric_water_offset=
        1U+8U+7U*8U+8U+8U+5U*8U;
    BinaryWriter nan;
    nan.pod(std::numeric_limits<double>::quiet_NaN());
    std::copy(
        nan.data().begin(),nan.data().end(),
        bytes.begin()+static_cast<std::ptrdiff_t>(
            first_atmospheric_water_offset
        )
    );
    bool rejected=false;
    try {
        BinaryReader reader(bytes);
        store.load(reader,store.snapshot_version());
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"climate store accepted non-finite atmospheric water");
    BinaryWriter after;
    store.save(after);
    check(
        after.data()==writer.data(),
        "failed climate store load mutated live state"
    );
}

} // namespace

int main() {
    try {
        optional_magic_contract();
        energy_and_moisture_accounting();
        seasonal_thermal_inertia();
        orographic_precipitation();
        lod_independent_reference_state();
        coupled_planet_water_and_snapshot();
        malformed_climate_store_is_rejected();
        std::cout<<"climate_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"climate_tests: FAIL: "<<error.what()<<'\n';
        return 1;
    }
}

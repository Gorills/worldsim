#include "worldsim/climate.hpp"
#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"
#include "worldsim/soil_nitrogen.hpp"

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

std::unique_ptr<Simulation> geography_climate_fixture(
    std::uint64_t seed,
    std::uint8_t level
) {
    auto simulation=std::make_unique<Simulation>(
        seed,SimulationConfig{level,level,86'400.0}
    );
    simulation->add_module(std::make_unique<GeographyModule>());
    simulation->add_module(std::make_unique<ClimateModule>());
    simulation->build();
    return simulation;
}

void run_vegetation(Simulation& simulation) {
    Scheduler scheduler;
    GeographyModule().register_systems(scheduler,simulation.fields());
    ClimateModule().register_systems(scheduler,simulation.fields());
    MagicModule().register_systems(scheduler,simulation.fields());
    HydrologyModule().register_systems(scheduler,simulation.fields());
    EcologyModule().register_systems(scheduler,simulation.fields());
    scheduler.finalize();
    SystemContext context{
        simulation.world(),simulation.fields(),1.0
    };
    for (ISimSystem* system:scheduler.order()) {
        if (system->id()=="ecology.vegetation") {
            system->step(context);
            return;
        }
    }
    throw std::runtime_error("vegetation system is absent");
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
        initial_heat+budget.absorbed_solar_j-budget.outgoing_longwave_j+
            budget.co2_forcing_j,
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

void snow_albedo_feedback() {
    near(
        snow_cover_fraction_from_swe(0.0),0.0,0.0,
        "zero SWE produced snow cover"
    );
    check(
        snow_cover_fraction_from_swe(0.10)>0.99,
        "deep SWE did not approach complete snow cover"
    );
    near(
        climate_land_albedo(0.0),0.28,1.0e-15,
        "snow-free land albedo changed"
    );
    near(
        climate_land_albedo(1.0),0.75,1.0e-15,
        "complete-snow land albedo changed"
    );
    auto bare=make_default_simulation(
        89,SimulationConfig{2,2,3'600.0}
    );
    auto snowy=make_default_simulation(
        89,SimulationConfig{2,2,3'600.0}
    );
    auto& snowy_fields=snowy->world().stores().get<FieldStore>();
    const FieldId snow=*snowy->fields().find(
        "hydrology.snow_water_m3"
    );
    const FieldId land=*snowy->fields().find(
        "geography.land_fraction"
    );
    for (CellId cell:snowy->world().active_cells()) {
        const double land_area=
            snowy->world().topology().area_m2(cell)*
            snowy_fields.get(cell,land);
        snowy_fields.set(cell,snow,0.10*land_area);
    }

    const double snowy_initial_heat=snowy->world().stores()
        .get<ClimateStore>().total_surface_heat_j();
    bare->step();
    snowy->step();
    const double bare_absorbed=bare->world().stores()
        .get<ClimateStore>().budget().absorbed_solar_j;
    const double snowy_absorbed=snowy->world().stores()
        .get<ClimateStore>().budget().absorbed_solar_j;
    check(
        snowy_absorbed<0.98*bare_absorbed,
        "snow water did not reduce absorbed shortwave radiation"
    );
    const ClimateStore& snowy_climate=
        snowy->world().stores().get<ClimateStore>();
    near(
        snowy_climate.total_surface_heat_j(),
        snowy_initial_heat+
            snowy_climate.budget().absorbed_solar_j-
            snowy_climate.budget().outgoing_longwave_j,
        2.0e-12,
        "snow-dependent radiation broke the energy ledger"
    );

    const FieldId snow_cover=*snowy->fields().find(
        "climate.snow_cover_fraction"
    );
    const FieldId surface_albedo=*snowy->fields().find(
        "climate.surface_albedo"
    );
    double maximum_cover=0.0;
    double maximum_albedo=0.0;
    for (CellId cell:snowy->world().active_cells()) {
        maximum_cover=std::max(
            maximum_cover,snowy_fields.get(cell,snow_cover)
        );
        maximum_albedo=std::max(
            maximum_albedo,snowy_fields.get(cell,surface_albedo)
        );
    }
    check(
        maximum_cover>0.99 && maximum_albedo>0.70,
        "deep snow did not project its cover/albedo diagnostics"
    );
}

void snow_coupling_is_lod_independent() {
    auto coarse=make_default_simulation(
        90,SimulationConfig{1,2,3'600.0}
    );
    auto focused=make_default_simulation(
        90,SimulationConfig{1,2,3'600.0}
    );
    const FieldId snow=*coarse->fields().find(
        "hydrology.snow_water_m3"
    );
    const FieldId land=*coarse->fields().find(
        "geography.land_fraction"
    );
    for (Simulation* simulation:{coarse.get(),focused.get()}) {
        auto& fields=simulation->world().stores().get<FieldStore>();
        for (CellId cell:simulation->world().active_cells()) {
            const double land_area=
                simulation->world().topology().area_m2(cell)*
                fields.get(cell,land);
            const double depth=0.01*static_cast<double>(
                1U+(cell.raw()%7U)
            );
            fields.set(cell,snow,depth*land_area);
        }
    }
    focused->set_focus({1.0,0.2,0.1});
    coarse->step();
    focused->step();

    const auto& a=coarse->world().stores().get<ClimateStore>();
    const auto& b=focused->world().stores().get<ClimateStore>();
    check(a.nodes().size()==b.nodes().size(),"focus changed climate grid");
    for (std::size_t i=0;i<a.nodes().size();++i) {
        near(
            a.nodes()[i].snow_cover_fraction,
            b.nodes()[i].snow_cover_fraction,
            2.0e-14,
            "focus LOD changed reference snow cover"
        );
        near(
            a.nodes()[i].land_temperature_k,
            b.nodes()[i].land_temperature_k,
            2.0e-14,
            "focus LOD changed snow-coupled land temperature"
        );
    }
}

void snow_burial_suppresses_short_vegetation() {
    auto bare=make_default_simulation(
        92,SimulationConfig{1,1,3'600.0}
    );
    auto snowy=make_default_simulation(
        92,SimulationConfig{1,1,3'600.0}
    );
    const CellId cell=*bare->world().active_cells().begin();
    const double area=bare->world().topology().area_m2(cell);
    for (Simulation* simulation:{bare.get(),snowy.get()}) {
        auto& fields=simulation->world().stores().get<FieldStore>();
        const auto set=[&](std::string_view key, double value) {
            fields.set(cell,*simulation->fields().find(key),value);
        };
        set("geography.land_fraction",1.0);
        set("geology.regolith_thickness_m",1.0);
        set(
            "hydrology.soil_water_m3",
            0.60*soil_water_capacity_depth_m(1.0)*area
        );
        set("hydrology.flooded_fraction",0.0);
        set("hydrology.inundation_days",0.0);
        set("climate.surface_temperature_k",286.0);
        set("climate.solar_flux_w_m2",340.0);
        set("ecology.soil_fertility",1.0);
        set("magic.growth_factor",1.0);
        set("ecology.grass_carbon_kg",0.20*area);
        set("ecology.shrub_carbon_kg",0.0);
        set("ecology.tree_carbon_kg",0.0);
        set("ecology.vegetation_carbon_kg",0.20*area);
        const double grass_nitrogen=
            0.20*area/kPlantCarbonNitrogenRatio[0];
        set("ecology.grass_nitrogen_kg",grass_nitrogen);
        set("ecology.shrub_nitrogen_kg",0.0);
        set("ecology.tree_nitrogen_kg",0.0);
        set("ecology.vegetation_nitrogen_kg",grass_nitrogen);
        set("ecology.mineral_nitrogen_kg",0.02*area);
    }
    bare->world().stores().get<FieldStore>().set(
        cell,*bare->fields().find("climate.snow_cover_fraction"),0.0
    );
    snowy->world().stores().get<FieldStore>().set(
        cell,*snowy->fields().find("climate.snow_cover_fraction"),1.0
    );

    run_vegetation(*bare);
    run_vegetation(*snowy);
    const double bare_npp=bare->world().stores().get<FieldStore>().get(
        cell,*bare->fields().find("ecology.npp_kg_day")
    );
    const double snowy_npp=snowy->world().stores().get<FieldStore>().get(
        cell,*snowy->fields().find("ecology.npp_kg_day")
    );
    check(
        bare_npp>snowy_npp+5.0e-4*area,
        "complete snow burial did not suppress grass production"
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

double flat_surface_l2_l3_temperature_mae_k() {
    auto coarse=climate_fixture(314,2,2,86'400.0);
    auto fine=climate_fixture(314,3,3,86'400.0);
    coarse->step(730);
    fine->step(730);

    const auto& coarse_store=
        coarse->world().stores().get<ClimateStore>();
    const auto& fine_store=
        fine->world().stores().get<ClimateStore>();
    struct Aggregate {
        double land_temperature_area{};
        double land_area{};
        double ocean_temperature_area{};
        double ocean_area{};
    };
    std::vector<Aggregate> aggregated(coarse_store.nodes().size());
    for (const ClimateNode& node:fine_store.nodes()) {
        const std::size_t index=coarse_store.node_index(node.cell);
        Aggregate& aggregate=aggregated[index];
        const double ocean_area=node.area_m2-node.land_area_m2;
        aggregate.land_temperature_area+=
            node.land_temperature_k*node.land_area_m2;
        aggregate.land_area+=node.land_area_m2;
        aggregate.ocean_temperature_area+=
            node.ocean_temperature_k*ocean_area;
        aggregate.ocean_area+=ocean_area;
    }

    double absolute_error_area=0.0;
    double total_area=0.0;
    for (std::size_t i=0;i<coarse_store.nodes().size();++i) {
        const ClimateNode& node=coarse_store.nodes()[i];
        const Aggregate& aggregate=aggregated[i];
        if (aggregate.land_area>0.0) {
            const double fine_temperature=
                aggregate.land_temperature_area/aggregate.land_area;
            absolute_error_area+=
                std::abs(node.land_temperature_k-fine_temperature)*
                aggregate.land_area;
            total_area+=aggregate.land_area;
        }
        if (aggregate.ocean_area>0.0) {
            const double fine_temperature=
                aggregate.ocean_temperature_area/aggregate.ocean_area;
            absolute_error_area+=
                std::abs(node.ocean_temperature_k-fine_temperature)*
                aggregate.ocean_area;
            total_area+=aggregate.ocean_area;
        }
    }
    return absolute_error_area/std::max(1.0,total_area);
}

double cumulative_flat_precipitation_m3(
    std::uint64_t seed,
    std::uint8_t level
) {
    auto simulation=climate_fixture(seed,level,level,86'400.0);
    simulation->step(365);
    const ClimateBudget& budget=
        simulation->world().stores().get<ClimateStore>().budget();
    return budget.land_precipitation_m3+budget.ocean_precipitation_m3;
}

void flat_moisture_transport_resolution_diagnostic() {
    double maximum_relative_delta=0.0;
    for (std::uint64_t seed:{0ULL,42ULL,999ULL}) {
        const double coarse=cumulative_flat_precipitation_m3(seed,2);
        const double fine=cumulative_flat_precipitation_m3(seed,3);
        const double relative_delta=
            std::abs(coarse-fine)/std::max({1.0,coarse,fine});
        maximum_relative_delta=std::max(
            maximum_relative_delta,
            relative_delta
        );
        std::cerr
            <<"flat moisture diagnostic: seed="<<seed
            <<" l2_precip_m3="<<coarse
            <<" l3_precip_m3="<<fine
            <<" relative_delta="<<relative_delta
            <<'\n';
    }
    check(
        maximum_relative_delta<1.0e-12,
        "flat L2/L3 cumulative precipitation divergence regressed"
    );
}

void horizontal_heat_transport_is_resolution_consistent() {
    const double error=flat_surface_l2_l3_temperature_mae_k();
    check(
        error<0.35,
        "flat-surface L2/L3 climate temperature diverged"
    );
}

void orographic_reference_elevation_is_resolution_consistent() {
    for (std::uint64_t seed:{0ULL,42ULL,999ULL}) {
        auto coarse=geography_climate_fixture(seed,2);
        auto fine=geography_climate_fixture(seed,3);
        const auto& coarse_store=
            coarse->world().stores().get<ClimateStore>();
        const auto& fine_store=
            fine->world().stores().get<ClimateStore>();

        struct Aggregate {
            double elevation_area{};
            double area{};
        };
        std::vector<Aggregate> aggregated(coarse_store.nodes().size());
        for (const ClimateNode& node:fine_store.nodes()) {
            const std::size_t index=coarse_store.node_index(node.cell);
            aggregated[index].elevation_area+=
                node.orographic_elevation_m*node.area_m2;
            aggregated[index].area+=node.area_m2;
        }

        double maximum_error_m=0.0;
        for (std::size_t i=0;i<coarse_store.nodes().size();++i) {
            const double fine_mean=
                aggregated[i].elevation_area/
                std::max(1.0,aggregated[i].area);
            maximum_error_m=std::max(
                maximum_error_m,
                std::abs(
                    coarse_store.nodes()[i].orographic_elevation_m-fine_mean
                )
            );
        }
        check(
            maximum_error_m<1.0e-6,
            "L2/L3 climate reference orography is not area-consistent"
        );
    }
}

void stochastic_weather_forcing_is_resolution_consistent() {
    for (std::uint64_t seed:{0ULL,42ULL,999ULL}) {
        auto coarse=climate_fixture(seed,2,2,86'400.0);
        auto fine=climate_fixture(seed,3,3,86'400.0);
        for (int step=0;step<8;++step) {
            coarse->step(1);
            fine->step(1);

            const auto& coarse_store=
                coarse->world().stores().get<ClimateStore>();
            const auto& fine_store=
                fine->world().stores().get<ClimateStore>();
            struct Aggregate {
                double anomaly_area{};
                double area{};
            };
            std::vector<Aggregate> aggregated(coarse_store.nodes().size());
            for (const ClimateNode& node:fine_store.nodes()) {
                const std::size_t index=coarse_store.node_index(node.cell);
                aggregated[index].anomaly_area+=
                    node.weather_anomaly_k*node.area_m2;
                aggregated[index].area+=node.area_m2;
            }

            double maximum_error_k=0.0;
            for (std::size_t i=0;i<coarse_store.nodes().size();++i) {
                const double fine_mean=
                    aggregated[i].anomaly_area/
                    std::max(1.0,aggregated[i].area);
                maximum_error_k=std::max(
                    maximum_error_k,
                    std::abs(
                        coarse_store.nodes()[i].weather_anomaly_k-fine_mean
                    )
                );
            }
            check(
                maximum_error_k<1.0e-12,
                "L2/L3 stochastic weather forcing is not area-consistent"
            );
        }
    }
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

void carbon_cycle_closes_and_forces_climate() {
    auto coupled=make_default_simulation(
        9191,SimulationConfig{1,1,3'600.0}
    );
    const double carbon_before=total_planet_carbon_kg(
        coupled->world(),coupled->fields()
    );
    coupled->step(24*30);
    const double carbon_after=total_planet_carbon_kg(
        coupled->world(),coupled->fields()
    );
    near(
        carbon_before,carbon_after,5.0e-13,
        "coupled atmosphere/ocean/ecology carbon inventory does not close"
    );

    auto forcing=climate_fixture(9192,1,1,86'400.0);
    auto& store=forcing->world().stores().get<ClimateStore>();
    const double initial_atmosphere=store.atmospheric_carbon_kg();
    const double initial_ocean=store.ocean_carbon_kg();
    store.advance_carbon(initial_atmosphere,0.0);
    near(
        store.atmospheric_co2_ppm(),560.0,1.0e-14,
        "doubling atmospheric carbon did not double CO2 ppm"
    );
    near(
        store.co2_radiative_forcing_w_m2(),
        5.35*std::log(2.0),1.0e-14,
        "CO2 forcing does not follow the logarithmic closure"
    );
    const double doubled_atmosphere=store.atmospheric_carbon_kg();
    store.advance_carbon(0.0,365.2422);
    check(
        store.atmospheric_carbon_kg()<doubled_atmosphere &&
        store.ocean_carbon_kg()>initial_ocean,
        "ocean carbon reservoir did not buffer an atmospheric perturbation"
    );
    near(
        store.atmospheric_carbon_kg()+store.ocean_carbon_kg(),
        initial_atmosphere*2.0+initial_ocean,2.0e-15,
        "air-sea carbon exchange is not conservative"
    );

    const double heat_before=store.total_surface_heat_j();
    const double absorbed_before=store.budget().absorbed_solar_j;
    const double outgoing_before=store.budget().outgoing_longwave_j;
    const double forcing_before=store.budget().co2_forcing_j;
    forcing->step(1);
    check(
        store.budget().co2_forcing_j>forcing_before,
        "positive CO2 perturbation did not enter the energy ledger"
    );
    near(
        store.total_surface_heat_j(),
        heat_before+
            (store.budget().absorbed_solar_j-absorbed_before)-
            (store.budget().outgoing_longwave_j-outgoing_before)+
            (store.budget().co2_forcing_j-forcing_before),
        3.0e-12,
        "CO2 forcing is not included in surface energy closure"
    );
}

void malformed_climate_store_is_rejected() {
    auto simulation=climate_fixture(303,1,1,3'600.0);
    auto& store=simulation->world().stores().get<ClimateStore>();
    BinaryWriter writer;
    store.save(writer);
    auto bytes=writer.data();
    // Header: reference level, three reservoir doubles, eight budget values
    // and count. The seventh node double is atmospheric water.
    constexpr std::size_t first_atmospheric_water_offset=
        1U+3U*8U+8U*8U+8U+8U+6U*8U;
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

    bytes=writer.data();
    // Orographic reference elevation is the fourth node double.
    constexpr std::size_t first_orographic_elevation_offset=
        1U+3U*8U+8U*8U+8U+8U+3U*8U;
    std::copy(
        nan.data().begin(),nan.data().end(),
        bytes.begin()+static_cast<std::ptrdiff_t>(
            first_orographic_elevation_offset
        )
    );
    rejected=false;
    try {
        BinaryReader reader(bytes);
        store.load(reader,store.snapshot_version());
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"climate store accepted invalid orographic elevation");
    BinaryWriter after_orography;
    store.save(after_orography);
    check(
        after_orography.data()==writer.data(),
        "failed orographic-elevation load mutated live climate state"
    );

    bytes=writer.data();
    // Atmospheric carbon follows reference level and ocean water.
    constexpr std::size_t atmospheric_carbon_offset=1U+8U;
    std::copy(
        nan.data().begin(),nan.data().end(),
        bytes.begin()+static_cast<std::ptrdiff_t>(
            atmospheric_carbon_offset
        )
    );
    rejected=false;
    try {
        BinaryReader reader(bytes);
        store.load(reader,store.snapshot_version());
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"climate store accepted invalid atmospheric carbon");
    BinaryWriter after_carbon;
    store.save(after_carbon);
    check(
        after_carbon.data()==writer.data(),
        "failed carbon load mutated live climate state"
    );

    bytes=writer.data();
    // The snow-cover fraction is the final (16th) node double.
    constexpr std::size_t first_snow_cover_offset=
        1U+3U*8U+8U*8U+8U+8U+15U*8U;
    std::copy(
        nan.data().begin(),nan.data().end(),
        bytes.begin()+static_cast<std::ptrdiff_t>(
            first_snow_cover_offset
        )
    );
    rejected=false;
    try {
        BinaryReader reader(bytes);
        store.load(reader,store.snapshot_version());
    } catch (const std::exception&) {
        rejected=true;
    }
    check(rejected,"climate store accepted invalid snow cover");
    BinaryWriter after_snow;
    store.save(after_snow);
    check(
        after_snow.data()==writer.data(),
        "failed snow-cover load mutated live climate state"
    );
}

} // namespace

int main() {
    try {
        optional_magic_contract();
        energy_and_moisture_accounting();
        seasonal_thermal_inertia();
        snow_albedo_feedback();
        snow_coupling_is_lod_independent();
        snow_burial_suppresses_short_vegetation();
        horizontal_heat_transport_is_resolution_consistent();
        flat_moisture_transport_resolution_diagnostic();
        orographic_precipitation();
        orographic_reference_elevation_is_resolution_consistent();
        stochastic_weather_forcing_is_resolution_consistent();
        lod_independent_reference_state();
        coupled_planet_water_and_snapshot();
        carbon_cycle_closes_and_forces_climate();
        malformed_climate_store_is_rejected();
        std::cout<<"climate_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"climate_tests: FAIL: "<<error.what()<<'\n';
        return 1;
    }
}

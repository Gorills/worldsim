#include "worldsim/modules.hpp"
#include "worldsim/geology.hpp"
#include "worldsim/terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>

namespace worldsim {
namespace {

FieldId require_field(const FieldRegistry& r, std::string_view key) {
    const auto id=r.find(key);
    if (!id) throw std::runtime_error("required field missing: "+std::string(key));
    return *id;
}

double soil_water_capacity_depth_m(double regolith_thickness_m) {
    constexpr double fractured_substrate_storage_m=0.02;
    constexpr double developed_soil_storage_m=0.28;
    constexpr double regolith_storage_scale_m=0.75;
    const double depth=std::max(0.0,regolith_thickness_m);
    return fractured_substrate_storage_m+
        developed_soil_storage_m*
        (-std::expm1(-depth/regolith_storage_scale_m));
}

struct GeologyFieldIds {
    FieldId elevation{};
    FieldId land_fraction{};
    FieldId crust_thickness{};
    FieldId crust_density{};
    FieldId continental_fraction{};
    FieldId lithosphere_age{};
    FieldId sediment_mass{};
    FieldId regolith_thickness{};
    FieldId erosion_rate{};
    FieldId trench_forcing{};
    FieldId volcanic_arc_forcing{};
    FieldId collision_forcing{};
    FieldId rift_forcing{};
    FieldId drainage_area{};
    FieldId drainage_discharge{};
    std::optional<FieldId> runoff;
};

GeologyFieldIds geology_fields(const FieldRegistry& r) {
    return {
        require_field(r,"geography.elevation_m"),
        require_field(r,"geography.land_fraction"),
        require_field(r,"geology.crust_thickness_m"),
        require_field(r,"geology.crust_density_kg_m3"),
        require_field(r,"geology.continental_fraction"),
        require_field(r,"geology.lithosphere_age_ma"),
        require_field(r,"geology.sediment_mass_kg"),
        require_field(r,"geology.regolith_thickness_m"),
        require_field(r,"geology.erosion_rate_m_yr"),
        require_field(r,"geology.trench_forcing"),
        require_field(r,"geology.volcanic_arc_forcing"),
        require_field(r,"geology.collision_forcing"),
        require_field(r,"geology.rift_forcing"),
        require_field(r,"geology.drainage_area_m2"),
        require_field(r,"geology.drainage_discharge_m3_day"),
        r.find("hydrology.runoff_m3_day")
    };
}

GeologyState read_geology_state(
    const FieldStore& fs,
    CellId cell,
    const GeologyFieldIds& ids
) {
    return {
        fs.get(cell,ids.crust_thickness),
        fs.get(cell,ids.crust_density),
        fs.get(cell,ids.continental_fraction),
        fs.get(cell,ids.lithosphere_age),
        fs.get(cell,ids.sediment_mass),
        fs.get(cell,ids.regolith_thickness)
    };
}

void write_geology_state(
    FieldStore& fs,
    CellId cell,
    const GeologyFieldIds& ids,
    const GeologyState& state
) {
    fs.set(cell,ids.crust_thickness,state.crust_thickness_m);
    fs.set(cell,ids.crust_density,state.crust_density_kg_m3);
    fs.set(cell,ids.continental_fraction,state.continental_fraction);
    fs.set(cell,ids.lithosphere_age,state.lithosphere_age_ma);
    fs.set(cell,ids.sediment_mass,state.sediment_mass_kg);
    fs.set(cell,ids.regolith_thickness,state.regolith_thickness_m);
}

double region_mean_elevation(
    const WorldState& world,
    const FieldStore& fs,
    FieldId elevation,
    CellId region
) {
    double mean=0.0;
    for (const ActiveCoverPart& part:world.resolve_active_cover(region))
        mean+=fs.get(part.cell,elevation)*part.weight;
    return mean;
}

double land_fraction_from_elevation(double elevation_m) {
    const double t=std::clamp((elevation_m+100.0)/200.0,0.0,1.0);
    return t*t*(3.0-2.0*t);
}

void update_geography_surface(
    WorldState& world,
    const FieldRegistry& r,
    const GeologyModel& geology
) {
    auto& fs=world.stores().get<FieldStore>();
    const GeologyFieldIds ids=geology_fields(r);
    std::map<CellId,double> local_elevation;
    for (CellId cell:world.active_cells()) {
        const GeologyState state=read_geology_state(fs,cell,ids);
        const Vec3d direction=world.topology().center_unit(cell);
        const BoundaryFeatureSample features=geology.boundary_features(
            state,
            direction
        );
        fs.set(cell,ids.trench_forcing,features.trench_forcing);
        fs.set(cell,ids.volcanic_arc_forcing,features.volcanic_arc_forcing);
        fs.set(cell,ids.collision_forcing,features.collision_forcing);
        fs.set(cell,ids.rift_forcing,features.rift_forcing);
        local_elevation.emplace(
            cell,
            geology.surface_elevation_m(
                state,
                direction,
                world.topology().area_m2(cell)
            )
        );
    }

    // A short-range elastic-load approximation spreads part of the local
    // isostatic response to adjacent columns. It is deliberately conservative:
    // the persistent mass state remains local, while only the derived surface
    // responds flexurally across the adaptive cover.
    constexpr double flexural_coupling=0.15;
    for (CellId cell:world.active_cells()) {
        double neighbor_sum=0.0;
        std::size_t neighbor_count=0;
        for (const auto& side:world.active_neighbors4(cell)) {
            double weighted_sum=0.0;
            double weight_sum=0.0;
            for (const ActiveCoverPart& part:side) {
                if (part.cell==cell) continue;
                weighted_sum+=
                    local_elevation.at(part.cell)*part.weight;
                weight_sum+=part.weight;
            }
            if (!(weight_sum>0.0)) continue;
            neighbor_sum+=weighted_sum/weight_sum;
            ++neighbor_count;
        }
        const double local=local_elevation.at(cell);
        const double flexed=neighbor_count>0
            ? (1.0-flexural_coupling)*local+
              flexural_coupling*neighbor_sum/static_cast<double>(neighbor_count)
            : local;
        fs.set(cell,ids.elevation,flexed);
        fs.set(cell,ids.land_fraction,land_fraction_from_elevation(flexed));
    }
}

void initialize_geology(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    const GeologyFieldIds ids=geology_fields(r);
    const GeologyModel geology(world.seed());
    for (CellId cell:world.active_cells()) {
        const GeologyState state=geology.initial_state(
            world.topology().center_unit(cell),
            world.topology().area_m2(cell)
        );
        write_geology_state(fs,cell,ids,state);
        fs.set(cell,ids.erosion_rate,0.0);
    }
    update_geography_surface(world,r,geology);
}

class GeologySystem final : public ISimSystem {
public:
    explicit GeologySystem(const FieldRegistry& r):
        ids_(geology_fields(r)),
        has_climate_(r.find("climate.surface_temperature_k").has_value()),
        has_ecology_(r.find("ecology.vegetation_carbon_kg").has_value()) {}

    std::string_view id() const override { return "geology.evolution"; }
    Tick cadence_ticks() const override { return 24; }

    std::vector<std::string> after() const override {
        // A runoff field may come from an external/test module without the
        // default ecology system. Depend on ecology.hydrology only when the
        // EcologyModule schema is actually present.
        if (has_ecology_) return {"ecology.hydrology"};
        if (has_climate_) return {"climate.surface"};
        return {};
    }

    SystemAccess access() const override {
        std::vector<std::string> reads{
            "field:geography.elevation_m",
            "field:geography.land_fraction",
            "field:geology.crust_thickness_m",
            "field:geology.crust_density_kg_m3",
            "field:geology.continental_fraction",
            "field:geology.lithosphere_age_ma",
            "field:geology.sediment_mass_kg",
            "field:geology.regolith_thickness_m"
        };
        if (ids_.runoff) reads.push_back("field:hydrology.runoff_m3_day");
        return {
            std::move(reads),
            {
                "field:geography.elevation_m",
                "field:geography.land_fraction",
                "field:geology.crust_thickness_m",
                "field:geology.crust_density_kg_m3",
                "field:geology.continental_fraction",
                "field:geology.lithosphere_age_ma",
                "field:geology.sediment_mass_kg",
                "field:geology.regolith_thickness_m",
                "field:geology.erosion_rate_m_yr",
                "field:geology.trench_forcing",
                "field:geology.volcanic_arc_forcing",
                "field:geology.collision_forcing",
                "field:geology.rift_forcing",
                "field:geology.drainage_area_m2",
                "field:geology.drainage_discharge_m3_day"
            }
        };
    }

    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        const GeologyModel geology(ctx.world.seed());
        const double dt_years=ctx.dt_days/365.2422;

        std::map<CellId,GeologyState> states;
        for (CellId cell:ctx.world.active_cells()) {
            GeologyState state=read_geology_state(fs,cell,ids_);
            geology.advance_tectonics(
                state,
                ctx.world.topology().center_unit(cell),
                dt_years
            );
            states.emplace(cell,state);
        }

        // Use post-tectonic, pre-erosion relief to route one conservative
        // sediment hop downslope. Updates are accumulated before deposition so
        // iteration order cannot create or destroy transported mass.
        for (const auto& [cell,state]:states)
            write_geology_state(fs,cell,ids_,state);
        update_geography_surface(ctx.world,ctx.fields,geology);

        struct FlowTarget {
            CellId cell;
            double weight{};
        };
        struct FlowRoute {
            std::vector<FlowTarget> targets;
            double slope{};
        };

        std::map<CellId,FlowRoute> routes;
        std::vector<CellId> elevation_order;
        elevation_order.reserve(ctx.world.active_cells().size());

        for (CellId cell:ctx.world.active_cells()) {
            elevation_order.push_back(cell);
            const double source_elevation=fs.get(cell,ids_.elevation);
            const Vec3d source_direction=ctx.world.topology().center_unit(cell);
            double downhill_elevation=source_elevation;
            double downhill_distance_m=1.0;
            std::optional<CellId> downhill_region;

            for (CellId same_level_neighbor:ctx.world.topology().neighbors4(cell)) {
                const double candidate=region_mean_elevation(
                    ctx.world,
                    fs,
                    ids_.elevation,
                    same_level_neighbor
                );
                if (candidate<downhill_elevation) {
                    downhill_region=same_level_neighbor;
                    downhill_elevation=candidate;
                    downhill_distance_m=kEarthRadiusM*std::acos(std::clamp(
                        dot(
                            source_direction,
                            ctx.world.topology().center_unit(same_level_neighbor)
                        ),
                        -1.0,
                        1.0
                    ));
                }
            }
            if (!downhill_region) continue;

            const auto parts=ctx.world.resolve_active_cover(
                *downhill_region
            );
            std::vector<FlowTarget> downhill_targets;
            double target_weight_sum=0.0;
            for (const ActiveCoverPart& part:parts) {
                if (part.cell==cell) continue;
                if (!(fs.get(part.cell,ids_.elevation)<source_elevation))
                    continue;
                downhill_targets.push_back({
                    part.cell,
                    part.weight
                });
                target_weight_sum+=part.weight;
            }
            if (!(target_weight_sum>0.0)) continue;

            FlowRoute route;
            route.slope=std::max(
                0.0,
                (source_elevation-downhill_elevation)/
                std::max(1.0,downhill_distance_m)
            );
            route.targets.reserve(downhill_targets.size());
            for (const FlowTarget& target:downhill_targets)
                route.targets.push_back({
                    target.cell,
                    target.weight/target_weight_sum
                });
            routes.emplace(cell,std::move(route));
        }

        // Accumulate catchment area and runoff through a strictly downhill
        // directed acyclic graph. Sorting by elevation guarantees all upstream
        // contributions reach a cell before it is propagated farther down.
        std::sort(
            elevation_order.begin(),
            elevation_order.end(),
            [&](CellId a, CellId b) {
                const double ea=fs.get(a,ids_.elevation);
                const double eb=fs.get(b,ids_.elevation);
                if (ea!=eb) return ea>eb;
                return a.raw()<b.raw();
            }
        );
        std::map<CellId,double> drainage_area;
        std::map<CellId,double> discharge;
        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            const double land=fs.get(cell,ids_.land_fraction);
            drainage_area[cell]=area*land;
            discharge[cell]=ids_.runoff
                ? fs.get(cell,*ids_.runoff)
                : 0.001*area*land;
        }
        for (CellId cell:elevation_order) {
            // Terrestrial drainage terminates at the first submerged receiver.
            // Marine sediment routing below may continue farther downslope, but
            // river discharge is not propagated across the ocean floor.
            if (fs.get(cell,ids_.elevation)<0.0) continue;
            const auto route=routes.find(cell);
            if (route==routes.end()) continue;
            for (const FlowTarget& target:route->second.targets) {
                drainage_area[target.cell]+=
                    drainage_area[cell]*target.weight;
                discharge[target.cell]+=
                    discharge[cell]*target.weight;
            }
        }
        for (CellId cell:ctx.world.active_cells()) {
            fs.set(cell,ids_.drainage_area,drainage_area[cell]);
            fs.set(cell,ids_.drainage_discharge,discharge[cell]);
        }

        std::map<CellId,double> deposits;
        for (CellId cell:ctx.world.active_cells()) {
            const auto route=routes.find(cell);
            if (route==routes.end()) {
                fs.set(cell,ids_.erosion_rate,0.0);
                continue;
            }

            const double area=ctx.world.topology().area_m2(cell);
            const double source_elevation=fs.get(cell,ids_.elevation);
            GeologyState& state=states.at(cell);

            double transport_rate=0.0;
            double transported_mass=0.0;
            if (source_elevation<0.0) {
                // Once sediment crosses sea level, stop applying terrestrial
                // runoff/hillslope incision. Existing marine sediment can
                // still move downslope through a sediment-only submarine path.
                const double sediment_depth=
                    geology.sediment_column_thickness_m(
                        state.sediment_mass_kg,
                        area
                    );
                transport_rate=
                    geology.marine_sediment_transport_rate_m_per_year(
                        route->second.slope,
                        -source_elevation,
                        sediment_depth
                    );
                const double transport_depth=std::min(
                    0.05,
                    transport_rate*dt_years
                );
                transported_mass=geology.entrain_sediment(
                    state,
                    area,
                    transport_depth
                );
            } else {
                const double runoff_m_day=
                    discharge[cell]/std::max(1.0,area);
                const double fluvial_erosion_rate=
                    geology.erosion_rate_m_per_year(
                        route->second.slope,
                        runoff_m_day,
                        state.regolith_thickness_m
                    );
                const double hillslope_transport_rate=
                    geology.hillslope_transport_rate_m_per_year(
                        route->second.slope,
                        state.regolith_thickness_m
                    );
                transport_rate=
                    fluvial_erosion_rate+hillslope_transport_rate;
                const double erosion_depth=std::min(
                    0.05,
                    transport_rate*dt_years
                );
                const ErosionBudget budget=geology.erode(
                    state,
                    area,
                    erosion_depth
                );
                transported_mass=budget.transported_mass_kg();
            }

            for (const FlowTarget& target:route->second.targets)
                deposits[target.cell]+=
                    transported_mass*target.weight;
            fs.set(cell,ids_.erosion_rate,transport_rate);
        }

        for (const auto& [cell,mass]:deposits)
            geology.deposit(states.at(cell),mass);
        for (const auto& [cell,state]:states)
            write_geology_state(fs,cell,ids_,state);

        update_geography_surface(ctx.world,ctx.fields,geology);
    }

private:
    GeologyFieldIds ids_;
    bool has_climate_{};
    bool has_ecology_{};
};

class MagicSystem final : public ISimSystem {
public:
    explicit MagicSystem(const FieldRegistry& r)
        : mana_(require_field(r,"magic.mana_j")), growth_(require_field(r,"magic.growth_factor")),
          temp_(require_field(r,"magic.temperature_anomaly_k")) {}
    std::string_view id() const override { return "magic.flux"; }
    Tick cadence_ticks() const override { return 6; }
    SystemAccess access() const override {
        return {{"field:magic.mana_j"},{"field:magic.mana_j","field:magic.growth_factor","field:magic.temperature_anomaly_k"}};
    }
    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        const double dt=ctx.dt_days;
        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            double mana=fs.get(cell,mana_);
            const double phase=deterministic_unit(ctx.world.seed(),fnv1a64("magic.phase"),0,cell.raw())*2.0*kPi;
            const double forcing=1.0+0.25*std::sin(phase+static_cast<double>(ctx.world.tick())*0.002);
            const double equilibrium=area*2.0e6*forcing;
            mana += (equilibrium-mana)*(1.0-std::exp(-0.02*dt));
            fs.set(cell,mana_,mana);
            const double density=mana/std::max(1.0,area);
            const double strength=std::tanh(density/2.0e6);
            fs.set(cell,growth_,1.0+0.35*strength);
            fs.set(cell,temp_,4.0*strength);
        }
    }
private:
    FieldId mana_,growth_,temp_;
};

class ClimateSystem final : public ISimSystem {
public:
    explicit ClimateSystem(const FieldRegistry& r)
        : elev_(require_field(r,"geography.elevation_m")), land_(require_field(r,"geography.land_fraction")),
          magic_temp_(require_field(r,"magic.temperature_anomaly_k")), temp_(require_field(r,"climate.surface_temperature_k")),
          precip_(require_field(r,"climate.precipitation_mm_day")), solar_(require_field(r,"climate.solar_flux_w_m2")),
          anomaly_(require_field(r,"climate.weather_anomaly_k")) {}
    std::string_view id() const override { return "climate.surface"; }
    std::vector<std::string> after() const override { return {"magic.flux"}; }
    SystemAccess access() const override {
        return {{"field:geography.elevation_m","field:geography.land_fraction","field:magic.temperature_anomaly_k","field:climate.weather_anomaly_k"},
                {"field:climate.surface_temperature_k","field:climate.precipitation_mm_day","field:climate.solar_flux_w_m2","field:climate.weather_anomaly_k"}};
    }
    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        const double day=static_cast<double>(ctx.world.tick())*ctx.dt_days;
        const double decl=23.44*kPi/180.0*std::sin(2.0*kPi*(day-80.0)/365.2422);
        for (CellId cell:ctx.world.active_cells()) {
            const auto [lat,lon]=ctx.world.topology().lat_lon_rad(cell); (void)lon;
            const double elev=fs.get(cell,elev_);
            const double land=fs.get(cell,land_);
            const double u=deterministic_unit(ctx.world.seed(),fnv1a64("climate.weather"),ctx.world.tick(),cell.raw());
            const double old_anom=fs.get(cell,anomaly_);
            const double decay=std::exp(-ctx.dt_days/2.0);
            const double anom=old_anom*decay+(u*2.0-1.0)*2.2*(1.0-decay);
            const double lat_cooling=42.0*std::pow(std::abs(std::sin(lat)),1.25);
            const double seasonal=10.0*std::sin(lat)*std::sin(2.0*kPi*(day-172.0)/365.2422);
            const double lapse=std::max(0.0,elev)*0.0065;
            const double t=301.0-lat_cooling+seasonal-lapse+anom+fs.get(cell,magic_temp_);
            const double insolation=340.0*std::max(0.08,std::cos(lat-decl));
            const double tropical=4.5*std::pow(std::cos(lat),2.0);
            const double stormbelt=2.5*std::exp(-std::pow((std::abs(lat)*180.0/kPi-50.0)/16.0,2.0));
            const double rain_shadow=std::exp(-std::max(0.0,elev)/5000.0);
            const double precip=(0.35+tropical+stormbelt)*(0.75+0.35*(1.0-land))*rain_shadow*std::clamp(1.0+anom*0.06,0.2,2.0);
            fs.set(cell,anomaly_,anom);
            fs.set(cell,temp_,t);
            fs.set(cell,solar_,insolation);
            fs.set(cell,precip_,precip);
        }
    }
private:
    FieldId elev_,land_,magic_temp_,temp_,precip_,solar_,anomaly_;
};

class HydrologySystem final : public ISimSystem {
public:
    explicit HydrologySystem(const FieldRegistry& r)
        : temp_(require_field(r,"climate.surface_temperature_k")),
          precip_(require_field(r,"climate.precipitation_mm_day")),
          land_(require_field(r,"geography.land_fraction")),
          regolith_(require_field(r,"geology.regolith_thickness_m")),
          water_(require_field(r,"hydrology.soil_water_m3")),
          runoff_(require_field(r,"hydrology.runoff_m3_day")) {}
    std::string_view id() const override { return "ecology.hydrology"; }
    Tick cadence_ticks() const override { return 6; }
    std::vector<std::string> after() const override { return {"climate.surface"}; }
    SystemAccess access() const override {
        return {{
                    "field:climate.surface_temperature_k",
                    "field:climate.precipitation_mm_day",
                    "field:geography.land_fraction",
                    "field:geology.regolith_thickness_m",
                    "field:hydrology.soil_water_m3"
                },
                {
                    "field:hydrology.soil_water_m3",
                    "field:hydrology.runoff_m3_day"
                }};
    }
    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            const double land=fs.get(cell,land_);
            const double effective_area=area*land;
            if (effective_area<=1.0) {
                fs.set(cell,water_,0.0);
                fs.set(cell,runoff_,0.0);
                continue;
            }

            double water=fs.get(cell,water_);
            const double rain=
                fs.get(cell,precip_)*0.001*effective_area*ctx.dt_days;
            const double temp=fs.get(cell,temp_);
            const double evap_depth=
                std::max(0.0,temp-258.0)*0.000035*ctx.dt_days;
            const double evap=std::min(
                water+rain,
                evap_depth*effective_area
            );
            water+=rain-evap;

            const double capacity=
                soil_water_capacity_depth_m(fs.get(cell,regolith_))*
                effective_area;
            const double excess=std::max(0.0,water-capacity);
            water-=excess;
            fs.set(cell,water_,water);
            fs.set(
                cell,
                runoff_,
                excess/std::max(ctx.dt_days,1e-12)
            );
        }
    }
private:
    FieldId temp_,precip_,land_,regolith_,water_,runoff_;
};

class SoilSystem final : public ISimSystem {
public:
    explicit SoilSystem(const FieldRegistry& r)
        : temp_(require_field(r,"climate.surface_temperature_k")),
          land_(require_field(r,"geography.land_fraction")),
          regolith_(require_field(r,"geology.regolith_thickness_m")),
          water_(require_field(r,"hydrology.soil_water_m3")),
          runoff_(require_field(r,"hydrology.runoff_m3_day")),
          fertility_(require_field(r,"ecology.soil_fertility")),
          litter_(require_field(r,"ecology.litter_carbon_kg")) {}

    std::string_view id() const override { return "ecology.soil"; }
    Tick cadence_ticks() const override { return 24; }
    std::vector<std::string> after() const override {
        return {"geology.evolution"};
    }
    SystemAccess access() const override {
        return {{
                    "field:climate.surface_temperature_k",
                    "field:geography.land_fraction",
                    "field:geology.regolith_thickness_m",
                    "field:hydrology.soil_water_m3",
                    "field:hydrology.runoff_m3_day",
                    "field:ecology.soil_fertility",
                    "field:ecology.litter_carbon_kg"
                },
                {
                    "field:ecology.soil_fertility",
                    "field:ecology.litter_carbon_kg"
                }};
    }

    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            const double land=fs.get(cell,land_);
            const double effective_area=area*land;
            if (effective_area<=1.0) {
                fs.set(cell,fertility_,0.0);
                fs.set(cell,litter_,0.0);
                continue;
            }

            const double regolith=std::max(
                0.0,
                fs.get(cell,regolith_)
            );
            const double capacity=
                soil_water_capacity_depth_m(regolith)*effective_area;
            const double moisture=std::clamp(
                fs.get(cell,water_)/
                    std::max(1.0,0.65*capacity),
                0.0,
                1.0
            );
            const double temp=fs.get(cell,temp_);
            const double temperature_factor=std::clamp(
                std::pow(2.0,(temp-283.0)/10.0),
                0.1,
                4.0
            );

            double litter=fs.get(cell,litter_);
            constexpr double base_decomposition_per_day=1.0e-3;
            const double decomposition_rate=
                base_decomposition_per_day*
                temperature_factor*
                (0.15+0.85*moisture);
            const double decomposed=litter*(
                1.0-std::exp(-decomposition_rate*ctx.dt_days)
            );
            litter=std::max(0.0,litter-decomposed);

            const double substrate_factor=
                -std::expm1(-regolith/0.75);
            const double litter_density=
                litter/std::max(1.0,effective_area);
            const double organic_factor=
                -std::expm1(-litter_density/0.75);
            const double target_fertility=std::clamp(
                0.05+
                0.60*substrate_factor+
                0.35*organic_factor,
                0.0,
                1.0
            );

            double fertility=std::clamp(
                fs.get(cell,fertility_),
                0.0,
                1.0
            );
            const double equilibration=
                1.0-std::exp(-ctx.dt_days/365.2422);
            fertility+=
                (target_fertility-fertility)*equilibration;

            // Decomposition gives a small mineralization pulse to the reduced
            // fertility state. This is deliberately an index-level feedback,
            // not an elemental N/P mass balance.
            fertility+=std::min(
                0.05,
                0.10*decomposed/std::max(1.0,effective_area)
            );

            // Strong drainage slowly leaches the reduced fertility state.
            const double runoff_depth=
                fs.get(cell,runoff_)*ctx.dt_days/
                std::max(1.0,effective_area);
            fertility*=std::exp(-0.5*std::max(0.0,runoff_depth));

            fs.set(
                cell,
                fertility_,
                std::clamp(fertility,0.0,1.0)
            );
            fs.set(cell,litter_,litter);
        }
    }

private:
    FieldId temp_,land_,regolith_,water_,runoff_,fertility_,litter_;
};

class VegetationSystem final : public ISimSystem {
public:
    explicit VegetationSystem(const FieldRegistry& r)
        : temp_(require_field(r,"climate.surface_temperature_k")),
          solar_(require_field(r,"climate.solar_flux_w_m2")),
          land_(require_field(r,"geography.land_fraction")),
          regolith_(require_field(r,"geology.regolith_thickness_m")),
          water_(require_field(r,"hydrology.soil_water_m3")),
          growth_(require_field(r,"magic.growth_factor")),
          fertility_(require_field(r,"ecology.soil_fertility")),
          litter_(require_field(r,"ecology.litter_carbon_kg")),
          pft_{
              require_field(r,"ecology.grass_carbon_kg"),
              require_field(r,"ecology.shrub_carbon_kg"),
              require_field(r,"ecology.tree_carbon_kg")
          },
          carbon_(require_field(r,"ecology.vegetation_carbon_kg")),
          npp_(require_field(r,"ecology.npp_kg_day")) {}

    std::string_view id() const override { return "ecology.vegetation"; }
    Tick cadence_ticks() const override { return 24; }
    std::vector<std::string> after() const override {
        return {"ecology.soil","magic.flux"};
    }
    SystemAccess access() const override {
        return {{
                    "field:climate.surface_temperature_k",
                    "field:climate.solar_flux_w_m2",
                    "field:geography.land_fraction",
                    "field:geology.regolith_thickness_m",
                    "field:hydrology.soil_water_m3",
                    "field:magic.growth_factor",
                    "field:ecology.soil_fertility",
                    "field:ecology.litter_carbon_kg",
                    "field:ecology.grass_carbon_kg",
                    "field:ecology.shrub_carbon_kg",
                    "field:ecology.tree_carbon_kg"
                },
                {
                    "field:ecology.litter_carbon_kg",
                    "field:ecology.grass_carbon_kg",
                    "field:ecology.shrub_carbon_kg",
                    "field:ecology.tree_carbon_kg",
                    "field:ecology.vegetation_carbon_kg",
                    "field:ecology.npp_kg_day"
                }};
    }

    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();

        // Snapshot PFT state before recruitment. Seed pressure must be based on
        // the start of the vegetation step so iteration order cannot propagate
        // a plant through multiple cells in one day.
        std::map<CellId,std::array<double,3>> before;
        for (CellId cell:ctx.world.active_cells()) {
            before[cell]={
                fs.get(cell,pft_[0]),
                fs.get(cell,pft_[1]),
                fs.get(cell,pft_[2])
            };
        }

        constexpr std::array<double,3> temp_opt{
            286.0,294.0,292.0
        };
        constexpr std::array<double,3> temp_width{
            24.0,20.0,16.0
        };
        constexpr std::array<double,3> productivity_kg_m2_day{
            1.10e-3,7.0e-4,5.5e-4
        };
        constexpr std::array<double,3> respiration_per_day{
            5.5e-4,3.5e-4,2.2e-4
        };
        constexpr std::array<double,3> turnover_per_day{
            1.20e-3,5.0e-4,1.8e-4
        };
        constexpr std::array<double,3> maximum_density_kg_m2{
            1.8,3.5,9.0
        };
        constexpr std::array<double,3> neighbor_establishment{
            0.50,0.28,0.16
        };
        constexpr double propagule_density_scale_kg_m2=0.05;

        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            const double land=fs.get(cell,land_);
            const double effective_area=area*land;
            if (effective_area<=1.0) {
                for (FieldId field:pft_) fs.set(cell,field,0.0);
                fs.set(cell,carbon_,0.0);
                fs.set(cell,npp_,0.0);
                continue;
            }

            std::array<double,3> local_density{};
            for (std::size_t i=0;i<pft_.size();++i)
                local_density[i]=
                    before.at(cell)[i]/effective_area;

            std::array<double,3> neighbor_density{};
            for (const auto& side:ctx.world.active_neighbors4(cell)) {
                std::array<double,3> side_density{};
                for (const ActiveCoverPart& part:side) {
                    const double neighbor_area=
                        ctx.world.topology().area_m2(part.cell)*
                        fs.get(part.cell,land_);
                    if (!(neighbor_area>1.0)) continue;
                    for (std::size_t i=0;i<pft_.size();++i)
                        side_density[i]+=
                            before.at(part.cell)[i]/
                            neighbor_area*
                            part.weight;
                }
                for (std::size_t i=0;i<pft_.size();++i)
                    neighbor_density[i]+=0.25*side_density[i];
            }

            const double temp=fs.get(cell,temp_);
            const double capacity=
                soil_water_capacity_depth_m(
                    fs.get(cell,regolith_)
                )*effective_area;
            const double moisture=std::clamp(
                fs.get(cell,water_)/
                    std::max(1.0,0.60*capacity),
                0.0,
                1.0
            );
            const double solar_factor=std::clamp(
                fs.get(cell,solar_)/340.0,
                0.05,
                1.2
            );
            const double fertility=std::clamp(
                fs.get(cell,fertility_),
                0.0,
                1.0
            );
            const double magic_growth=fs.get(cell,growth_);

            const std::array<double,3> moisture_factor{
                0.25+0.75*moisture,
                std::clamp(
                    0.25+0.75*std::exp(
                        -std::pow((moisture-0.55)/0.50,2.0)
                    ),
                    0.0,
                    1.0
                ),
                std::pow(moisture,1.35)
            };
            const std::array<double,3> fertility_factor{
                0.35+0.65*fertility,
                0.25+0.75*fertility,
                0.10+0.90*fertility
            };

            const double total_density=
                local_density[0]+
                local_density[1]+
                local_density[2];
            const double shared_space=std::clamp(
                1.0-total_density/9.0,
                0.0,
                1.0
            );
            const std::array<double,3> light_factor{
                std::exp(
                    -0.35*local_density[1]-
                    0.75*local_density[2]
                ),
                std::exp(-0.35*local_density[2]),
                1.0
            };

            double total_after=0.0;
            double total_npp_rate=0.0;
            double litter_addition=0.0;

            for (std::size_t i=0;i<pft_.size();++i) {
                const double temperature_factor=std::exp(
                    -std::pow(
                        (temp-temp_opt[i])/temp_width[i],
                        2.0
                    )
                );
                const double local_propagules=
                    1.0-std::exp(
                        -local_density[i]/
                        propagule_density_scale_kg_m2
                    );
                const double neighbor_propagules=
                    1.0-std::exp(
                        -neighbor_density[i]/
                        propagule_density_scale_kg_m2
                    );
                const double establishment=std::clamp(
                    local_propagules+
                    neighbor_establishment[i]*neighbor_propagules,
                    0.0,
                    1.0
                );
                const double own_space=std::clamp(
                    1.0-
                    local_density[i]/
                    maximum_density_kg_m2[i],
                    0.0,
                    1.0
                );

                const double gross_rate=
                    effective_area*
                    productivity_kg_m2_day[i]*
                    temperature_factor*
                    moisture_factor[i]*
                    fertility_factor[i]*
                    solar_factor*
                    magic_growth*
                    establishment*
                    shared_space*
                    own_space*
                    light_factor[i];

                const double temperature_respiration=std::clamp(
                    std::pow(2.0,(temp-283.0)/10.0),
                    0.2,
                    4.0
                );
                const double respiration_rate=
                    before.at(cell)[i]*
                    respiration_per_day[i]*
                    temperature_respiration;
                const double npp_rate=
                    gross_rate-respiration_rate;

                const double turnover=
                    before.at(cell)[i]*
                    (
                        1.0-
                        std::exp(
                            -turnover_per_day[i]*ctx.dt_days
                        )
                    );
                const double unconstrained=
                    before.at(cell)[i]+
                    npp_rate*ctx.dt_days-
                    turnover;
                const double maximum_carbon=
                    effective_area*maximum_density_kg_m2[i];
                const double updated=std::clamp(
                    unconstrained,
                    0.0,
                    maximum_carbon
                );
                const double crowding_loss=std::max(
                    0.0,
                    unconstrained-updated
                );

                fs.set(cell,pft_[i],updated);
                total_after+=updated;
                total_npp_rate+=npp_rate;
                litter_addition+=turnover+crowding_loss;
            }

            fs.add(cell,litter_,litter_addition);
            fs.set(cell,carbon_,total_after);
            fs.set(cell,npp_,total_npp_rate);
        }
    }

private:
    FieldId temp_,solar_,land_,regolith_,water_,growth_;
    FieldId fertility_,litter_;
    std::array<FieldId,3> pft_;
    FieldId carbon_,npp_;
};

class FaunaSystem final : public ISimSystem {
public:
    explicit FaunaSystem(const FieldRegistry& r)
        : land_(require_field(r,"geography.land_fraction")),
          pft_{
              require_field(r,"ecology.grass_carbon_kg"),
              require_field(r,"ecology.shrub_carbon_kg"),
              require_field(r,"ecology.tree_carbon_kg")
          },
          carbon_(require_field(r,"ecology.vegetation_carbon_kg")),
          litter_(require_field(r,"ecology.litter_carbon_kg")) {}

    std::string_view id() const override { return "ecology.fauna"; }
    Tick cadence_ticks() const override { return 24; }
    std::vector<std::string> after() const override {
        return {"ecology.vegetation"};
    }
    SystemAccess access() const override {
        return {{
                    "field:geography.land_fraction",
                    "field:ecology.grass_carbon_kg",
                    "field:ecology.shrub_carbon_kg",
                    "field:ecology.tree_carbon_kg",
                    "field:ecology.vegetation_carbon_kg",
                    "field:ecology.litter_carbon_kg",
                    "store:ecology.cohorts"
                },
                {
                    "field:ecology.grass_carbon_kg",
                    "field:ecology.shrub_carbon_kg",
                    "field:ecology.tree_carbon_kg",
                    "field:ecology.vegetation_carbon_kg",
                    "field:ecology.litter_carbon_kg",
                    "store:ecology.cohorts"
                }};
    }

    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        auto& cs=ctx.world.stores().get<CohortStore>();
        constexpr std::array<double,3> forage_preference{
            1.0,0.55,0.12
        };

        // First update local trophic dynamics. Migration is deliberately a
        // second phase so every movement decision observes the same
        // post-feeding population snapshot.
        for (CellId cell:ctx.world.active_cells()) {
            auto cohorts=cs.in_cell(cell);
            std::vector<Cohort*> herbivores,carnivores;
            for (auto& ref:cohorts) {
                Cohort& cohort=ref.get();
                if (cohort.functional_group==1)
                    herbivores.push_back(&cohort);
                else if (cohort.functional_group==2)
                    carnivores.push_back(&cohort);
            }

            std::array<double,3> vegetation{
                fs.get(cell,pft_[0]),
                fs.get(cell,pft_[1]),
                fs.get(cell,pft_[2])
            };
            double weighted_forage=0.0;
            for (std::size_t i=0;i<pft_.size();++i)
                weighted_forage+=
                    vegetation[i]*forage_preference[i];

            double total_demand=0.0;
            for (const Cohort* herbivore:herbivores)
                total_demand+=
                    herbivore->count*
                    herbivore->body_mass_kg*
                    0.018*
                    ctx.dt_days;

            const double consumed=std::min(
                total_demand,
                weighted_forage*0.025
            );
            if (weighted_forage>0.0 && consumed>0.0) {
                for (std::size_t i=0;i<pft_.size();++i) {
                    const double share=
                        vegetation[i]*
                        forage_preference[i]/
                        weighted_forage;
                    vegetation[i]=std::max(
                        0.0,
                        vegetation[i]-consumed*share
                    );
                }
            }

            // A reduced fraction of grazed plant carbon returns immediately
            // as fecal/unassimilated detritus.
            fs.add(cell,litter_,0.35*consumed);
            const double food_ratio=total_demand>0.0
                ? std::clamp(consumed/total_demand,0.0,1.0)
                : 1.0;
            for (Cohort* herbivore:herbivores) {
                const double rate=
                    0.0045*food_ratio-
                    0.006*(1.0-food_ratio);
                herbivore->count=std::max(
                    0.0,
                    herbivore->count*
                    std::exp(rate*ctx.dt_days)
                );
            }

            double prey_biomass=0.0;
            for (const Cohort* herbivore:herbivores)
                prey_biomass+=
                    herbivore->count*
                    herbivore->body_mass_kg;
            double pred_demand=0.0;
            for (const Cohort* carnivore:carnivores)
                pred_demand+=
                    carnivore->count*
                    carnivore->body_mass_kg*
                    0.025*
                    ctx.dt_days;
            const double killed=std::min(
                pred_demand,
                prey_biomass*0.003
            );
            if (prey_biomass>0.0 && killed>0.0) {
                fs.add(cell,litter_,0.12*killed);
                const double survival=std::clamp(
                    1.0-killed/prey_biomass,
                    0.0,
                    1.0
                );
                for (Cohort* herbivore:herbivores)
                    herbivore->count*=survival;
            }

            const double pred_food=pred_demand>0.0
                ? std::clamp(killed/pred_demand,0.0,1.0)
                : 1.0;
            for (Cohort* carnivore:carnivores) {
                const double before=carnivore->count;
                const double rate=
                    0.0035*pred_food-
                    0.007*(1.0-pred_food);
                carnivore->count=std::max(
                    0.0,
                    carnivore->count*
                    std::exp(rate*ctx.dt_days)
                );
                if (
                    before>=1.0 &&
                    carnivore->count<1.0
                ) {
                    ctx.world.emit({
                        ctx.world.tick(),
                        "cohort.near_extinction",
                        cell,
                        carnivore->id,
                        carnivore->count
                    });
                }
            }

            double total_vegetation=0.0;
            for (std::size_t i=0;i<pft_.size();++i) {
                fs.set(cell,pft_[i],vegetation[i]);
                total_vegetation+=vegetation[i];
            }
            fs.set(cell,carbon_,total_vegetation);
        }

        struct HabitatState {
            double herbivore{};
            double carnivore{};
        };
        std::map<CellId,HabitatState> habitat;
        for (CellId cell:ctx.world.active_cells()) {
            const double effective_area=
                ctx.world.topology().area_m2(cell)*
                fs.get(cell,land_);
            if (!(effective_area>1.0)) {
                habitat[cell]={0.0,0.0};
                continue;
            }

            double forage=0.0;
            for (std::size_t i=0;i<pft_.size();++i)
                forage+=
                    fs.get(cell,pft_[i])*forage_preference[i];
            const double forage_density=
                forage/effective_area;
            const double herbivore_quality=
                fs.get(cell,land_)*
                (
                    1.0-
                    std::exp(-forage_density/0.50)
                );

            double prey_biomass=0.0;
            for (const auto& ref:cs.in_cell(cell)) {
                const Cohort& cohort=ref.get();
                if (cohort.functional_group==1)
                    prey_biomass+=
                        cohort.count*cohort.body_mass_kg;
            }
            const double prey_density=
                prey_biomass/effective_area;
            const double carnivore_quality=
                fs.get(cell,land_)*
                (
                    1.0-
                    std::exp(-prey_density/2.5e-5)
                );
            habitat[cell]={
                std::clamp(herbivore_quality,0.0,1.0),
                std::clamp(carnivore_quality,0.0,1.0)
            };
        }

        struct PlannedMove {
            std::uint64_t cohort_id{};
            CellId target;
            double count{};
        };
        std::vector<Cohort> movement_snapshot;
        movement_snapshot.reserve(cs.all().size());
        for (const auto& [id,cohort]:cs.all()) {
            (void)id;
            if (
                cohort.count>0.0 &&
                (
                    cohort.functional_group==1 ||
                    cohort.functional_group==2
                )
            ) {
                movement_snapshot.push_back(cohort);
            }
        }

        std::vector<PlannedMove> moves;
        for (const Cohort& cohort:movement_snapshot) {
            const bool herbivore=cohort.functional_group==1;
            const auto quality_of=[&](CellId cell) {
                const HabitatState& h=habitat.at(cell);
                return herbivore ? h.herbivore : h.carnivore;
            };
            const double source_quality=quality_of(cohort.cell);

            const auto sides=ctx.world.active_neighbors4(cohort.cell);
            const std::vector<ActiveCoverPart>* best_side=nullptr;
            double best_quality=source_quality;
            for (const auto& side:sides) {
                double side_quality=0.0;
                for (const ActiveCoverPart& part:side)
                    side_quality+=
                        quality_of(part.cell)*part.weight;
                if (side_quality>best_quality) {
                    best_quality=side_quality;
                    best_side=&side;
                }
            }

            constexpr double minimum_quality_gain=0.05;
            if (
                best_side==nullptr ||
                best_quality-source_quality<minimum_quality_gain
            ) {
                continue;
            }

            const double gradient=std::clamp(
                (best_quality-source_quality)/
                    std::max(0.10,best_quality),
                0.0,
                1.0
            );
            const double maximum_daily_fraction=
                herbivore ? 0.20 : 0.12;
            const double moved_fraction=
                1.0-
                std::exp(
                    -maximum_daily_fraction*
                    gradient*
                    ctx.dt_days
                );
            const double moved_count=
                cohort.count*
                std::clamp(moved_fraction,0.0,0.50);
            if (!(moved_count>0.0)) continue;

            double destination_weight_sum=0.0;
            for (const ActiveCoverPart& part:*best_side)
                destination_weight_sum+=
                    part.weight*
                    std::max(0.0,quality_of(part.cell));
            if (!(destination_weight_sum>0.0)) continue;

            double planned_sum=0.0;
            for (std::size_t i=0;i<best_side->size();++i) {
                const ActiveCoverPart& part=(*best_side)[i];
                const double destination_weight=
                    part.weight*
                    std::max(0.0,quality_of(part.cell));
                if (!(destination_weight>0.0)) continue;
                const double share=
                    destination_weight/destination_weight_sum;
                const double transfer=
                    std::min(
                        moved_count-planned_sum,
                        moved_count*share
                    );
                if (transfer>0.0) {
                    moves.push_back({
                        cohort.id,
                        part.cell,
                        transfer
                    });
                    planned_sum+=transfer;
                }
            }
        }

        // Applying the already-frozen move plan through CohortStore keeps the
        // cell index coherent and prevents arrivals from moving again during
        // this fauna step.
        for (const PlannedMove& move:moves)
            cs.transfer_count(
                move.cohort_id,
                move.target,
                move.count
            );
    }

private:
    FieldId land_;
    std::array<FieldId,3> pft_;
    FieldId carbon_,litter_;
};

} // namespace

void GeographyModule::register_fields(FieldRegistry& r) {
    r.register_field({"geography.elevation_m","m",FieldSemantics::Intensive,0.0,-11000.0,9000.0});
    r.register_field({"geography.land_fraction","1",FieldSemantics::Intensive,0.5,0.0,1.0});
    r.register_field({"geology.crust_thickness_m","m",FieldSemantics::Intensive,35'000.0,3'000.0,70'000.0});
    r.register_field({"geology.crust_density_kg_m3","kg/m3",FieldSemantics::Intensive,2'850.0,2'500.0,3'300.0});
    r.register_field({"geology.continental_fraction","1",FieldSemantics::Intensive,0.5,0.0,1.0});
    r.register_field({"geology.lithosphere_age_ma","Ma",FieldSemantics::Intensive,100.0,0.0,4'500.0});
    r.register_field({"geology.sediment_mass_kg","kg",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"geology.regolith_thickness_m","m",FieldSemantics::Intensive,0.0,0.0,100.0});
    r.register_field({"geology.erosion_rate_m_yr","m/yr",FieldSemantics::Intensive,0.0,0.0,0.1});
    r.register_field({"geology.trench_forcing","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"geology.volcanic_arc_forcing","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"geology.collision_forcing","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"geology.rift_forcing","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"geology.drainage_area_m2","m2",FieldSemantics::Intensive,0.0,0.0,1.0e30});
    r.register_field({"geology.drainage_discharge_m3_day","m3/day",FieldSemantics::Intensive,0.0,0.0,1.0e30});
}

void GeographyModule::register_systems(Scheduler& s, const FieldRegistry& r) {
    s.add(std::make_unique<GeologySystem>(r));
}

void GeographyModule::initialize(WorldState& world, const FieldRegistry& r) {
    initialize_geology(world,r);
}

void GeographyModule::on_spatial_cover_changed(WorldState& world, const FieldRegistry& r) {
    const GeologyModel geology(world.seed());
    update_geography_surface(world,r,geology);
}

void MagicModule::register_fields(FieldRegistry& r) {
    r.register_field({"magic.mana_j","J",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"magic.growth_factor","1",FieldSemantics::Intensive,1.0,0.1,5.0});
    r.register_field({"magic.temperature_anomaly_k","K",FieldSemantics::Intensive,0.0,-30.0,30.0});
}
void MagicModule::register_systems(Scheduler& s, const FieldRegistry& r) { s.add(std::make_unique<MagicSystem>(r)); }
void MagicModule::initialize(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    const auto mana=require_field(r,"magic.mana_j");
    for (CellId c:world.active_cells()) {
        const double area=world.topology().area_m2(c);
        const double variation=0.65+0.7*deterministic_unit(world.seed(),fnv1a64("magic.initial"),0,c.raw());
        fs.set(c,mana,area*2.0e6*variation);
    }
}

void ClimateModule::register_fields(FieldRegistry& r) {
    r.register_field({"climate.surface_temperature_k","K",FieldSemantics::Intensive,288.0,150.0,360.0});
    r.register_field({"climate.precipitation_mm_day","mm/day",FieldSemantics::Intensive,2.0,0.0,1000.0});
    r.register_field({"climate.solar_flux_w_m2","W/m2",FieldSemantics::Intensive,250.0,0.0,1500.0});
    r.register_field({"climate.weather_anomaly_k","K",FieldSemantics::Intensive,0.0,-30.0,30.0});
}
void ClimateModule::register_systems(Scheduler& s, const FieldRegistry& r) { s.add(std::make_unique<ClimateSystem>(r)); }
void ClimateModule::initialize(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    const auto temp=require_field(r,"climate.surface_temperature_k");
    const auto precip=require_field(r,"climate.precipitation_mm_day");
    const auto solar=require_field(r,"climate.solar_flux_w_m2");
    const auto elev=require_field(r,"geography.elevation_m");
    for (CellId c:world.active_cells()) {
        const auto [lat,lon]=world.topology().lat_lon_rad(c); (void)lon;
        fs.set(c,temp,301.0-42.0*std::pow(std::abs(std::sin(lat)),1.25)-std::max(0.0,fs.get(c,elev))*0.0065);
        fs.set(c,precip,0.5+4.5*std::pow(std::cos(lat),2.0));
        fs.set(c,solar,340.0*std::max(0.08,std::cos(lat)));
    }
}

void EcologyModule::register_fields(FieldRegistry& r) {
    r.register_field({"hydrology.soil_water_m3","m3",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"hydrology.runoff_m3_day","m3/day",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.soil_fertility","1",FieldSemantics::Intensive,0.25,0.0,1.0});
    r.register_field({"ecology.litter_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.grass_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.shrub_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.tree_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.vegetation_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.npp_kg_day","kgC/day",FieldSemantics::Extensive,0.0,-1.0e30,1.0e30});
}
void EcologyModule::register_stores(StateStoreRegistry& stores, const FieldRegistry&) { stores.emplace<CohortStore>(); }
void EcologyModule::register_systems(Scheduler& s, const FieldRegistry& r) {
    s.add(std::make_unique<HydrologySystem>(r));
    s.add(std::make_unique<SoilSystem>(r));
    s.add(std::make_unique<VegetationSystem>(r));
    s.add(std::make_unique<FaunaSystem>(r));
}
void EcologyModule::initialize(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    auto& cs=world.stores().get<CohortStore>();
    const auto land=require_field(r,"geography.land_fraction");
    const auto temp=require_field(r,"climate.surface_temperature_k");
    const auto regolith=require_field(r,"geology.regolith_thickness_m");
    const auto water=require_field(r,"hydrology.soil_water_m3");
    const auto fertility=require_field(r,"ecology.soil_fertility");
    const auto litter=require_field(r,"ecology.litter_carbon_kg");
    const std::array<FieldId,3> pft{
        require_field(r,"ecology.grass_carbon_kg"),
        require_field(r,"ecology.shrub_carbon_kg"),
        require_field(r,"ecology.tree_carbon_kg")
    };
    const auto carbon=require_field(r,"ecology.vegetation_carbon_kg");

    for (CellId c:world.active_cells()) {
        const double area=world.topology().area_m2(c);
        const double lf=fs.get(c,land);
        const double effective=area*lf;
        const double regolith_depth=std::max(
            0.0,
            fs.get(c,regolith)
        );
        const double substrate_factor=
            -std::expm1(-regolith_depth/0.75);
        const double soil_fertility=std::clamp(
            0.05+0.70*substrate_factor,
            0.0,
            1.0
        );
        const double suitability=std::exp(
            -std::pow((fs.get(c,temp)-291.0)/24.0,2.0)
        );
        const double initial_water=
            effective*
            soil_water_capacity_depth_m(regolith_depth)*
            0.45;
        const double initial_carbon=
            effective*4.0*suitability*
            (0.25+0.75*soil_fertility);

        const std::array<double,3> pft_score{
            std::exp(
                -std::pow((fs.get(c,temp)-286.0)/24.0,2.0)
            )*(0.80-0.20*soil_fertility),
            std::exp(
                -std::pow((fs.get(c,temp)-294.0)/20.0,2.0)
            )*(0.45+0.25*soil_fertility),
            std::exp(
                -std::pow((fs.get(c,temp)-292.0)/16.0,2.0)
            )*(0.10+0.90*soil_fertility)
        };
        const double pft_score_sum=
            pft_score[0]+pft_score[1]+pft_score[2];

        fs.set(c,water,initial_water);
        fs.set(c,fertility,soil_fertility);
        double pft_total=0.0;
        for (std::size_t i=0;i<pft.size();++i) {
            const double pool=pft_score_sum>0.0
                ? initial_carbon*pft_score[i]/pft_score_sum
                : 0.0;
            fs.set(c,pft[i],pool);
            pft_total+=pool;
        }
        fs.set(c,carbon,pft_total);
        fs.set(c,litter,0.08*pft_total);

        if (lf>0.2 && suitability>0.12 && soil_fertility>0.08) {
            const double km2=effective/1.0e6;
            const double habitat=
                suitability*(0.30+0.70*soil_fertility);
            cs.add({
                0,0,c,1,1,
                std::max(10.0,km2*0.7*habitat),
                35.0,2.0
            });
            cs.add({
                0,0,c,2,2,
                std::max(2.0,km2*0.015*habitat),
                70.0,4.0
            });
        }
    }
}

} // namespace worldsim

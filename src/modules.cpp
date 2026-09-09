#include "worldsim/modules.hpp"
#include "worldsim/climate.hpp"
#include "worldsim/geology.hpp"
#include "worldsim/hydrology.hpp"
#include "worldsim/soil_carbon.hpp"
#include "worldsim/terrain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
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
        has_climate_exchange_(
            r.find("climate.atmospheric_water_m3").has_value()
        ),
        has_hydrology_(r.find("hydrology.surface_water_m3").has_value()) {}

    std::string_view id() const override { return "geology.evolution"; }
    Tick cadence_ticks() const override { return 24; }

    std::vector<std::string> after() const override {
        if (has_climate_exchange_)
            return {"climate.surface_exchange"};
        if (has_hydrology_) return {"hydrology.balance"};
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
        if (!has_hydrology_ && ids_.runoff) reads.push_back("field:hydrology.runoff_m3_day");
        std::vector<std::string> coupled_writes;
        if (has_hydrology_) {
            reads.push_back("store:hydrology.basins");
            coupled_writes.push_back("store:hydrology.basins");
            for (const auto* key:{"surface_water_m3","surface_depth_m","surface_level_m","flooded_fraction", "river_discharge_m3_day","basin_id","lake_id","spill_elevation_m"})
                coupled_writes.push_back("field:hydrology."+std::string(key));
        }
        SystemAccess result{
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
        result.writes.insert(result.writes.end(),coupled_writes.begin(),coupled_writes.end());
        return result;
    }

    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        const GeologyModel geology(ctx.world.seed());
        const double dt_years=ctx.dt_days/365.2422;
        std::map<CellId,double> bed_before,land_before;
        if (has_hydrology_)
            for (CellId cell:ctx.world.active_cells()) {
                bed_before[cell]=fs.get(cell,ids_.elevation);
                land_before[cell]=fs.get(cell,ids_.land_fraction);
            }

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

        std::map<CellId,double> discharge;
        if (has_hydrology_) {
            ctx.world.stores().get<HydrologyStore>().consume_geology_discharge(ctx.world,ctx.fields);
            for (CellId cell:ctx.world.active_cells()) discharge[cell]=fs.get(cell,ids_.drainage_discharge);
        } else {
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
        if (has_hydrology_) {
            for (auto& [cell,height]:bed_before) height=fs.get(cell,ids_.elevation)-height;
            for (auto& [cell,land]:land_before) land=fs.get(cell,ids_.land_fraction)-land;
            auto& hydrology=ctx.world.stores().get<HydrologyStore>();
            hydrology.apply_bed_changes(ctx.world,bed_before,land_before);
            hydrology.project(ctx.world,ctx.fields);
        }
    }

private:
    GeologyFieldIds ids_;
    bool has_climate_{};
    bool has_climate_exchange_{};
    bool has_hydrology_{};
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
        constexpr double forcing_angular_rate_rad_per_day=0.048;
        const double base_tick_days=
            dt/static_cast<double>(cadence_ticks());
        const double elapsed_days=
            (static_cast<double>(ctx.world.tick())+1.0)*
            base_tick_days;
        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            double mana=fs.get(cell,mana_);
            const double phase=deterministic_unit(ctx.world.seed(),fnv1a64("magic.phase"),0,cell.raw())*2.0*kPi;
            const double forcing=1.0+0.25*std::sin(
                phase+elapsed_days*forcing_angular_rate_rad_per_day
            );
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

class SoilSystem final : public ISimSystem {
public:
    explicit SoilSystem(const FieldRegistry& r)
        : temp_(require_field(r,"climate.surface_temperature_k")),
          land_(require_field(r,"geography.land_fraction")),
          regolith_(require_field(r,"geology.regolith_thickness_m")),
          water_(require_field(r,"hydrology.soil_water_m3")),
          runoff_(require_field(r,"hydrology.drainage_since_soil_m3")),
          fertility_(require_field(r,"ecology.soil_fertility")),
          litter_(require_field(r,"ecology.litter_carbon_kg")),
          fast_carbon_(
              require_field(r,"ecology.soil_fast_carbon_kg")
          ),
          slow_carbon_(
              require_field(r,"ecology.soil_slow_carbon_kg")
          ),
          soil_carbon_(require_field(r,"ecology.soil_carbon_kg")),
          heterotrophic_respiration_(require_field(
              r,"ecology.heterotrophic_respiration_kg_day"
          )),
          respired_carbon_(
              require_field(r,"ecology.soil_respired_carbon_kg")
          ) {}

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
                    "field:hydrology.drainage_since_soil_m3",
                    "field:ecology.soil_fertility",
                    "field:ecology.litter_carbon_kg",
                    "field:ecology.soil_fast_carbon_kg",
                    "field:ecology.soil_slow_carbon_kg",
                    "field:ecology.soil_respired_carbon_kg"
                },
                {
                    "field:ecology.soil_fertility",
                    "field:ecology.litter_carbon_kg",
                    "field:ecology.soil_fast_carbon_kg",
                    "field:ecology.soil_slow_carbon_kg",
                    "field:ecology.soil_carbon_kg",
                    "field:ecology.heterotrophic_respiration_kg_day",
                    "field:ecology.soil_respired_carbon_kg",
                    "field:hydrology.drainage_since_soil_m3"
                }};
    }

    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            const double land=fs.get(cell,land_);
            const double effective_area=area*land;
            const double drained=fs.get(cell,runoff_);
            fs.set(cell,runoff_,0.0);
            if (effective_area<=1.0) {
                fs.set(cell,fertility_,0.0);
                fs.set(cell,heterotrophic_respiration_,0.0);
                fs.set(
                    cell,
                    soil_carbon_,
                    fs.get(cell,fast_carbon_)+
                    fs.get(cell,slow_carbon_)
                );
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
            const SoilCarbonStep carbon=carbon_model_.advance(
                {
                    fs.get(cell,litter_),
                    fs.get(cell,fast_carbon_),
                    fs.get(cell,slow_carbon_)
                },
                fs.get(cell,temp_),
                moisture,
                ctx.dt_days
            );
            fs.set(cell,litter_,carbon.state.litter_carbon_kg);
            fs.set(cell,fast_carbon_,carbon.state.fast_carbon_kg);
            fs.set(cell,slow_carbon_,carbon.state.slow_carbon_kg);
            fs.set(
                cell,
                soil_carbon_,
                carbon.state.fast_carbon_kg+
                carbon.state.slow_carbon_kg
            );
            fs.set(
                cell,
                heterotrophic_respiration_,
                ctx.dt_days>0.0
                    ? carbon.fluxes.respired_kg/ctx.dt_days
                    : 0.0
            );
            fs.add(
                cell,
                respired_carbon_,
                carbon.fluxes.respired_kg
            );

            const double substrate_factor=
                -std::expm1(-regolith/0.75);
            const double litter_density=
                carbon.state.litter_carbon_kg/
                std::max(1.0,effective_area);
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
            const double decomposed=
                carbon.fluxes.litter_decomposed_kg+
                carbon.fluxes.fast_decomposed_kg+
                carbon.fluxes.slow_decomposed_kg;
            fertility+=std::min(
                0.05,
                0.10*decomposed/std::max(1.0,effective_area)
            );

            // Strong drainage slowly leaches the reduced fertility state.
            const double runoff_depth=
                drained/
                std::max(1.0,effective_area);
            fertility*=std::exp(-0.5*std::max(0.0,runoff_depth));

            fs.set(
                cell,
                fertility_,
                std::clamp(fertility,0.0,1.0)
            );
        }
    }

private:
    FieldId temp_,land_,regolith_,water_,runoff_,fertility_,litter_;
    FieldId fast_carbon_,slow_carbon_,soil_carbon_;
    FieldId heterotrophic_respiration_,respired_carbon_;
    SoilCarbonModel carbon_model_;
};

class VegetationSystem final : public ISimSystem {
public:
    explicit VegetationSystem(const FieldRegistry& r)
        : temp_(require_field(r,"climate.surface_temperature_k")),
          solar_(require_field(r,"climate.solar_flux_w_m2")),
          land_(require_field(r,"geography.land_fraction")),
          regolith_(require_field(r,"geology.regolith_thickness_m")),
          water_(require_field(r,"hydrology.soil_water_m3")),
          flooded_(require_field(r,"hydrology.flooded_fraction")),
          inundation_(require_field(r,"hydrology.inundation_days")),
          snow_cover_(
              require_field(r,"climate.snow_cover_fraction")
          ),
          growth_(r.find("magic.growth_factor")),
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
        auto dependencies=std::vector<std::string>{"ecology.soil"};
        if (growth_) dependencies.push_back("magic.flux");
        return dependencies;
    }
    SystemAccess access() const override {
        SystemAccess result{{
                    "field:climate.surface_temperature_k",
                    "field:climate.solar_flux_w_m2",
                    "field:geography.land_fraction",
                    "field:geology.regolith_thickness_m",
                    "field:hydrology.soil_water_m3",
                    "field:hydrology.flooded_fraction",
                    "field:hydrology.inundation_days",
                    "field:climate.snow_cover_fraction",
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
        if (growth_)
            result.reads.push_back("field:magic.growth_factor");
        return result;
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
            const double magic_growth=growth_
                ? fs.get(cell,*growth_)
                : 1.0;
            const double flooded=fs.get(cell,flooded_);
            const double snow_cover=std::clamp(
                fs.get(cell,snow_cover_),0.0,1.0
            );
            const double flood_mortality=0.02*flooded*(-std::expm1(-fs.get(cell,inundation_)/5.0));

            // Snow buries short vegetation more completely than woody
            // canopies. These are reduced exposure factors; snow mass and
            // cover remain owned by hydrology and climate respectively.
            const std::array<double,3> snow_exposure{
                1.0-snow_cover,
                1.0-0.50*snow_cover,
                1.0-0.15*snow_cover
            };

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
                    light_factor[i]*
                    snow_exposure[i]*
                    (1.0-flooded);

                const double temperature_respiration=std::clamp(
                    std::pow(2.0,(temp-283.0)/10.0),
                    0.2,
                    4.0
                );
                const double respiration_rate=
                    before.at(cell)[i]*
                    respiration_per_day[i]*
                    temperature_respiration;
                const double requested_turnover=
                    before.at(cell)[i]*
                    (
                        1.0-
                        std::exp(
                            -(turnover_per_day[i]+flood_mortality)*ctx.dt_days
                        )
                    );
                // Respiration and turnover draw from the same carbon pool.
                // Limit them jointly before reporting NPP: clamping only the
                // final stock can otherwise spend unavailable carbon on long
                // timesteps while still reporting the full respiratory loss.
                const double available=before.at(cell)[i]+gross_rate*ctx.dt_days;
                const double requested_respiration=respiration_rate*ctx.dt_days;
                const double requested_loss=requested_respiration+requested_turnover;
                const double loss_scale=requested_loss>available ? available/requested_loss : 1.0;
                const double turnover=requested_turnover*loss_scale;
                const double respiration=requested_respiration*loss_scale;
                const double npp_rate=gross_rate-respiration/ctx.dt_days;
                const double unconstrained=std::max(0.0,available-respiration-turnover);
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
    FieldId temp_,solar_,land_,regolith_,water_,flooded_,inundation_;
    FieldId snow_cover_;
    std::optional<FieldId> growth_;
    FieldId fertility_,litter_;
    std::array<FieldId,3> pft_;
    FieldId carbon_,npp_;
};

class FireSystem final : public ISimSystem {
public:
    explicit FireSystem(const FieldRegistry& r)
        : temp_(require_field(r,"climate.surface_temperature_k")),
          precipitation_(
              require_field(r,"climate.precipitation_mm_day")
          ),
          humidity_(require_field(r,"climate.relative_humidity")),
          east_wind_(require_field(r,"climate.wind_east_m_s")),
          north_wind_(require_field(r,"climate.wind_north_m_s")),
          land_(require_field(r,"geography.land_fraction")),
          regolith_(require_field(r,"geology.regolith_thickness_m")),
          water_(require_field(r,"hydrology.soil_water_m3")),
          flooded_(require_field(r,"hydrology.flooded_fraction")),
          snow_cover_(
              require_field(r,"climate.snow_cover_fraction")
          ),
          litter_(require_field(r,"ecology.litter_carbon_kg")),
          pft_{
              require_field(r,"ecology.grass_carbon_kg"),
              require_field(r,"ecology.shrub_carbon_kg"),
              require_field(r,"ecology.tree_carbon_kg")
          },
          carbon_(require_field(r,"ecology.vegetation_carbon_kg")),
          active_area_(require_field(r,"ecology.fire_active_area_m2")),
          active_(require_field(r,"ecology.fire_active_fraction")),
          danger_(require_field(r,"ecology.fire_danger")),
          burned_(require_field(r,"ecology.fire_burned_fraction")),
          burned_area_(require_field(r,"ecology.fire_burned_area_m2")),
          emitted_(
              require_field(r,"ecology.fire_emitted_carbon_kg")
          ),
          emission_rate_(
              require_field(r,"ecology.fire_emission_kg_day")
          ),
          char_(require_field(r,"ecology.pyrogenic_carbon_kg")) {}

    std::string_view id() const override { return "ecology.fire"; }
    Tick cadence_ticks() const override { return 24; }
    std::vector<std::string> after() const override {
        return {"ecology.vegetation"};
    }
    SystemAccess access() const override {
        return {{
                    "field:climate.surface_temperature_k",
                    "field:climate.precipitation_mm_day",
                    "field:climate.relative_humidity",
                    "field:climate.wind_east_m_s",
                    "field:climate.wind_north_m_s",
                    "field:geography.land_fraction",
                    "field:geology.regolith_thickness_m",
                    "field:hydrology.soil_water_m3",
                    "field:hydrology.flooded_fraction",
                    "field:climate.snow_cover_fraction",
                    "field:ecology.litter_carbon_kg",
                    "field:ecology.grass_carbon_kg",
                    "field:ecology.shrub_carbon_kg",
                    "field:ecology.tree_carbon_kg",
                    "field:ecology.vegetation_carbon_kg",
                    "field:ecology.fire_active_area_m2",
                    "field:ecology.fire_burned_area_m2",
                    "field:ecology.fire_emitted_carbon_kg",
                    "field:ecology.pyrogenic_carbon_kg"
                },
                {
                    "field:ecology.litter_carbon_kg",
                    "field:ecology.grass_carbon_kg",
                    "field:ecology.shrub_carbon_kg",
                    "field:ecology.tree_carbon_kg",
                    "field:ecology.vegetation_carbon_kg",
                    "field:ecology.fire_active_area_m2",
                    "field:ecology.fire_active_fraction",
                    "field:ecology.fire_danger",
                    "field:ecology.fire_burned_fraction",
                    "field:ecology.fire_burned_area_m2",
                    "field:ecology.fire_emitted_carbon_kg",
                    "field:ecology.fire_emission_kg_day",
                    "field:ecology.pyrogenic_carbon_kg"
                }};
    }

    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        for (CellId cell:ctx.world.active_cells())
            fs.set(cell,emission_rate_,0.0);

        struct FireState {
            double land_area_m2{};
            double danger{};
            double east_wind_m_s{};
            double north_wind_m_s{};
            double burned_fraction{};
            double next_active_fraction{};
        };
        std::map<CellId,FireState> state;

        constexpr double lower_fuel_density_kg_m2=0.03;
        constexpr double upper_fuel_density_kg_m2=0.35;
        constexpr double ignition_hazard_m2_day=2.0e-16;
        constexpr double minimum_ignition_area_m2=2.0e7;
        constexpr double maximum_ignition_area_m2=8.0e7;
        constexpr double maximum_daily_burn_fraction=0.25;
        const double maximum_burn_fraction=std::clamp(
            maximum_daily_burn_fraction*ctx.dt_days,
            0.0,
            1.0
        );
        const std::uint64_t ignition_stream=fnv1a64(
            "ecology.fire.natural_ignition"
        );
        const std::uint64_t size_stream=fnv1a64(
            "ecology.fire.ignition_area"
        );

        // Freeze danger and today's burning fraction before applying either
        // biomass loss or neighbor spread. Newly spread fire cannot move a
        // second spatial step during this pass.
        for (CellId cell:ctx.world.active_cells()) {
            FireState local;
            const double area=ctx.world.topology().area_m2(cell);
            local.land_area_m2=area*fs.get(cell,land_);
            local.east_wind_m_s=fs.get(cell,east_wind_);
            local.north_wind_m_s=fs.get(cell,north_wind_);

            if (!(local.land_area_m2>1.0)) {
                fs.set(cell,danger_,0.0);
                fs.set(cell,burned_,0.0);
                fs.set(cell,active_area_,0.0);
                fs.set(cell,active_,0.0);
                state.emplace(cell,local);
                continue;
            }

            const double surface_fuel=
                fs.get(cell,litter_)+
                fs.get(cell,pft_[0])+
                0.55*fs.get(cell,pft_[1])+
                0.25*fs.get(cell,pft_[2]);
            const double fuel_density=
                surface_fuel/local.land_area_m2;
            const double fuel_availability=std::clamp(
                (fuel_density-lower_fuel_density_kg_m2)/
                    (
                        upper_fuel_density_kg_m2-
                        lower_fuel_density_kg_m2
                    ),
                0.0,
                1.0
            );
            const double capacity=
                soil_water_capacity_depth_m(
                    fs.get(cell,regolith_)
                )*local.land_area_m2;
            const double root_zone_wetness=std::clamp(
                fs.get(cell,water_)/std::max(1.0,0.65*capacity),
                0.0,
                1.0
            );
            const double soil_dryness=1.0-root_zone_wetness;
            const double humidity_dryness=std::clamp(
                (0.85-fs.get(cell,humidity_))/0.65,
                0.0,
                1.0
            );
            const double temperature_factor=std::clamp(
                (fs.get(cell,temp_)-273.15)/20.0,
                0.0,
                1.0
            );
            const double rain_suppression=std::exp(
                -std::max(0.0,fs.get(cell,precipitation_))/5.0
            );
            local.danger=std::clamp(
                fuel_availability*
                std::pow(
                    0.70*soil_dryness+0.30*humidity_dryness,
                    1.5
                )*
                temperature_factor*
                rain_suppression*
                (1.0-fs.get(cell,snow_cover_))*
                (1.0-fs.get(cell,flooded_)),
                0.0,
                1.0
            );
            fs.set(cell,danger_,local.danger);

            double active=std::clamp(
                fs.get(cell,active_area_)/local.land_area_m2,
                0.0,
                1.0
            );
            bool natural_ignition=false;
            if (local.danger>0.0) {
                const double hazard=
                    ignition_hazard_m2_day*
                    local.land_area_m2*
                    std::pow(local.danger,4.0)*
                    ctx.dt_days;
                const double probability=-std::expm1(-hazard);
                const double draw=deterministic_unit(
                    ctx.world.seed(),
                    ignition_stream,
                    ctx.world.tick(),
                    cell.raw()
                );
                if (draw<probability) {
                    const double size_draw=deterministic_unit(
                        ctx.world.seed(),
                        size_stream,
                        ctx.world.tick(),
                        cell.raw()
                    );
                    const double ignition_area=
                        minimum_ignition_area_m2+
                        (
                            maximum_ignition_area_m2-
                            minimum_ignition_area_m2
                        )*size_draw;
                    active=std::max(
                        active,
                        std::min(
                            1.0,
                            ignition_area/local.land_area_m2
                        )
                    );
                    natural_ignition=true;
                }
            }

            const double wind_speed=std::hypot(
                local.east_wind_m_s,
                local.north_wind_m_s
            );
            if (active>0.0 && local.danger>0.0) {
                const double growth_rate=
                    0.25+
                    0.65*local.danger+
                    0.015*std::min(20.0,wind_speed);
                local.burned_fraction=std::clamp(
                    active*std::exp(
                        std::min(3.0,growth_rate*ctx.dt_days)
                    )*local.danger,
                    0.0,
                    maximum_burn_fraction
                );
            }
            fs.set(cell,burned_,local.burned_fraction);
            const double daily_persistence=std::clamp(
                0.10+0.70*local.danger,
                0.0,
                0.80
            );
            const double persistence=std::pow(
                daily_persistence,
                ctx.dt_days
            );
            local.next_active_fraction=
                local.burned_fraction*persistence;
            if (natural_ignition && local.burned_fraction>0.0) {
                ctx.world.emit({
                    ctx.world.tick(),
                    "ecology.fire_ignited",
                    cell,
                    0,
                    local.land_area_m2*local.burned_fraction
                });
            }
            state.emplace(cell,local);
        }

        constexpr std::array<double,3> mortality_fraction{
            0.92,0.70,0.35
        };
        constexpr std::array<double,3> combustion_fraction{
            0.80,0.60,0.35
        };
        constexpr std::array<double,3> char_fraction{
            0.03,0.05,0.07
        };
        constexpr double litter_char_fraction=0.08;

        for (CellId cell:ctx.world.active_cells()) {
            const FireState& local=state.at(cell);
            const double fraction=local.burned_fraction;
            if (!(fraction>0.0)) continue;

            const double litter_before=fs.get(cell,litter_);
            double litter_after=litter_before;
            double emitted=0.0;
            double charred=0.0;
            double vegetation_after=0.0;
            for (std::size_t i=0;i<pft_.size();++i) {
                const double before=fs.get(cell,pft_[i]);
                const double killed=std::min(
                    before,
                    before*fraction*mortality_fraction[i]
                );
                const double combusted=killed*combustion_fraction[i];
                const double pft_char=killed*char_fraction[i];
                litter_after+=killed-combusted-pft_char;
                emitted+=combusted;
                charred+=pft_char;
                const double after=before-killed;
                fs.set(cell,pft_[i],after);
                vegetation_after+=after;
            }

            const double litter_affected=std::min(
                litter_before,
                litter_before*fraction*(0.55+0.40*local.danger)
            );
            litter_after-=litter_affected;
            emitted+=litter_affected*(1.0-litter_char_fraction);
            charred+=litter_affected*litter_char_fraction;

            fs.set(cell,litter_,std::max(0.0,litter_after));
            fs.set(cell,carbon_,vegetation_after);
            fs.add(cell,emitted_,emitted);
            fs.add(cell,emission_rate_,emitted/ctx.dt_days);
            fs.add(cell,char_,charred);
            fs.add(
                cell,
                burned_area_,
                local.land_area_m2*fraction
            );
        }

        // Spread a bounded ignition area rather than copying a source-cell
        // fraction into differently sized neighbors. This keeps the coarse /
        // fine transfer scale explicit while the active-cover weights select
        // the actual leaves representing each neighboring region.
        std::map<CellId,double> incoming_active_area_m2;
        for (CellId source:ctx.world.active_cells()) {
            const FireState& local=state.at(source);
            if (!(local.burned_fraction>0.0)) continue;

            const Vec3d position=ctx.world.topology().center_unit(source);
            Vec3d east=normalized(Vec3d{-position.y,position.x,0.0});
            if (std::abs(position.x)+std::abs(position.y)<1.0e-12)
                east={0.0,1.0,0.0};
            const Vec3d north=normalized(cross(position,east));
            const Vec3d wind=
                east*local.east_wind_m_s+
                north*local.north_wind_m_s;
            const double wind_speed=norm(wind);
            const double burned_area=
                local.land_area_m2*local.burned_fraction;

            for (const auto& side:ctx.world.active_neighbors4(source)) {
                for (const ActiveCoverPart& part:side) {
                    const FireState& target=state.at(part.cell);
                    if (!(target.danger>0.15)) continue;

                    const Vec3d target_position=
                        ctx.world.topology().center_unit(part.cell);
                    const Vec3d toward=normalized(
                        target_position-
                        position*dot(position,target_position)
                    );
                    const double alignment=wind_speed>1.0e-12
                        ? dot(wind,toward)/wind_speed
                        : 0.0;
                    const double directional=std::clamp(
                        1.0+0.75*alignment,
                        0.25,
                        1.75
                    );
                    const double spread_area=
                        burned_area*
                        0.0125*
                        local.danger*
                        target.danger*
                        directional*
                        part.weight*
                        std::min(2.0,ctx.dt_days);
                    incoming_active_area_m2[part.cell]+=spread_area;
                }
            }
        }

        for (CellId cell:ctx.world.active_cells()) {
            FireState& local=state.at(cell);
            const double incoming=local.land_area_m2>1.0
                ? incoming_active_area_m2[cell]/local.land_area_m2
                : 0.0;
            fs.set(
                cell,
                active_,
                std::clamp(
                    local.next_active_fraction+incoming,
                    0.0,
                    maximum_burn_fraction
                )
            );
            fs.set(
                cell,
                active_area_,
                fs.get(cell,active_)*local.land_area_m2
            );
        }
    }

private:
    FieldId temp_,precipitation_,humidity_,east_wind_,north_wind_;
    FieldId land_,regolith_,water_,flooded_,snow_cover_,litter_;
    std::array<FieldId,3> pft_;
    FieldId carbon_,active_area_,active_,danger_,burned_,burned_area_;
    FieldId emitted_,emission_rate_,char_;
};

// Reduced standing-biomass pyramid used to bound the two demo trophic guilds.
// These are model closure parameters, not Earth-calibrated species claims.
constexpr double kHerbivoreCarbonPerWeightedForage=1.0e-4;
constexpr double kCarnivoreCarbonPerHerbivoreCarbon=0.08;
constexpr double kInitialHerbivoreCapacityFraction=0.10;

class FaunaSystem final : public ISimSystem {
public:
    FaunaSystem(const FieldRegistry& r, bool fire_enabled)
        : land_(require_field(r,"geography.land_fraction")),
          pft_{
              require_field(r,"ecology.grass_carbon_kg"),
              require_field(r,"ecology.shrub_carbon_kg"),
              require_field(r,"ecology.tree_carbon_kg")
          },
          carbon_(require_field(r,"ecology.vegetation_carbon_kg")),
          litter_(require_field(r,"ecology.litter_carbon_kg")),
          respired_(require_field(
              r,"ecology.fauna_respired_carbon_kg"
          )),
          respiration_rate_(require_field(
              r,"ecology.fauna_respiration_kg_day"
          )),
          fire_enabled_(fire_enabled) {}

    std::string_view id() const override { return "ecology.fauna"; }
    Tick cadence_ticks() const override { return 24; }
    std::vector<std::string> after() const override {
        return {
            fire_enabled_ ? "ecology.fire" : "ecology.vegetation"
        };
    }
    SystemAccess access() const override {
        return {{
                    "field:geography.land_fraction",
                    "field:ecology.grass_carbon_kg",
                    "field:ecology.shrub_carbon_kg",
                    "field:ecology.tree_carbon_kg",
                    "field:ecology.vegetation_carbon_kg",
                    "field:ecology.litter_carbon_kg",
                    "field:ecology.fauna_respired_carbon_kg",
                    "store:ecology.cohorts"
                },
                {
                    "field:ecology.grass_carbon_kg",
                    "field:ecology.shrub_carbon_kg",
                    "field:ecology.tree_carbon_kg",
                    "field:ecology.vegetation_carbon_kg",
                    "field:ecology.litter_carbon_kg",
                    "field:ecology.fauna_respired_carbon_kg",
                    "field:ecology.fauna_respiration_kg_day",
                    "store:ecology.cohorts"
                }};
    }

    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        auto& cs=ctx.world.stores().get<CohortStore>();
        for (CellId cell:ctx.world.active_cells())
            fs.set(cell,respiration_rate_,0.0);
        constexpr std::array<double,3> forage_preference{
            1.0,0.55,0.12
        };
        constexpr double herbivore_assimilation=0.25;
        constexpr double carnivore_assimilation=0.75;
        constexpr double herbivore_maintenance_per_day=0.0025;
        constexpr double carnivore_maintenance_per_day=0.0030;
        constexpr double herbivore_max_growth_per_day=0.00050;
        constexpr double carnivore_max_growth_per_day=0.00025;
        constexpr double herbivore_mortality_per_day=0.00020;
        constexpr double carnivore_mortality_per_day=0.00030;
        constexpr double density_regulation_per_day=0.010;

        const auto group_carbon=[](const std::vector<Cohort*>& cohorts) {
            double carbon=0.0;
            for (const Cohort* cohort:cohorts)
                carbon+=cohort_carbon_kg(*cohort);
            return carbon;
        };
        const auto scale_group=[](
            const std::vector<Cohort*>& cohorts,
            double before_carbon,
            double after_carbon
        ) {
            if (!(before_carbon>0.0)) return;
            const double scale=std::clamp(
                after_carbon/before_carbon,
                0.0,
                std::numeric_limits<double>::max()
            );
            for (Cohort* cohort:cohorts)
                cohort->count=std::max(0.0,cohort->count*scale);
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

            const double herbivore_carbon_before=
                group_carbon(herbivores);
            const double herbivore_capacity=
                weighted_forage*kHerbivoreCarbonPerWeightedForage;
            const double herbivore_capacity_ratio=
                herbivore_capacity>0.0
                    ? herbivore_carbon_before/herbivore_capacity
                    : std::numeric_limits<double>::infinity();
            const double herbivore_net_growth_per_day=
                herbivore_max_growth_per_day*
                std::clamp(1.0-herbivore_capacity_ratio,0.0,1.0);
            const double herbivore_maintenance=
                herbivore_carbon_before*
                herbivore_maintenance_per_day*
                ctx.dt_days;
            const double herbivore_growth_budget=
                herbivore_carbon_before*
                (
                    herbivore_mortality_per_day+
                    herbivore_net_growth_per_day
                )*
                ctx.dt_days;
            const double total_demand=
                (herbivore_maintenance+herbivore_growth_budget)/
                herbivore_assimilation;

            const double consumed=std::min(
                total_demand,
                weighted_forage*std::clamp(
                    0.025*ctx.dt_days,
                    0.0,
                    1.0
                )
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

            // Every demographic gain is paid by assimilated forage carbon.
            // Maintenance is respired; unassimilated food and background
            // mortality return to litter. This closes the local fauna transfer
            // against vegetation, litter, cohort biomass and the ledger.
            const double herbivore_assimilated=
                consumed*herbivore_assimilation;
            const double herbivore_respired=std::min(
                herbivore_carbon_before+herbivore_assimilated,
                herbivore_maintenance
            );
            const double herbivore_after_metabolism=std::max(
                0.0,
                herbivore_carbon_before+
                    herbivore_assimilated-
                    herbivore_respired
            );
            const double herbivore_background_mortality=
                herbivore_after_metabolism*
                (-std::expm1(
                    -herbivore_mortality_per_day*ctx.dt_days
                ));
            const double herbivore_after_background=std::max(
                0.0,
                herbivore_after_metabolism-
                    herbivore_background_mortality
            );
            const double herbivore_excess_fraction=
                herbivore_after_background>0.0
                    ? std::clamp(
                        (herbivore_after_background-herbivore_capacity)/
                            herbivore_after_background,
                        0.0,
                        1.0
                    )
                    : 0.0;
            const double herbivore_density_mortality=
                herbivore_after_background*
                (-std::expm1(
                    -density_regulation_per_day*
                    herbivore_excess_fraction*
                    ctx.dt_days
                ));
            const double herbivore_carbon_after=std::max(
                0.0,
                herbivore_after_background-
                    herbivore_density_mortality
            );
            fs.add(
                cell,
                litter_,
                consumed-herbivore_assimilated+
                    herbivore_background_mortality+
                    herbivore_density_mortality
            );
            fs.add(cell,respired_,herbivore_respired);
            fs.add(
                cell,respiration_rate_,
                herbivore_respired/ctx.dt_days
            );
            scale_group(
                herbivores,
                herbivore_carbon_before,
                herbivore_carbon_after
            );

            const double prey_biomass=group_carbon(herbivores);
            const double carnivore_carbon_before=
                group_carbon(carnivores);
            const double carnivore_capacity=
                prey_biomass*kCarnivoreCarbonPerHerbivoreCarbon;
            const double carnivore_capacity_ratio=
                carnivore_capacity>0.0
                    ? carnivore_carbon_before/carnivore_capacity
                    : std::numeric_limits<double>::infinity();
            const double carnivore_net_growth_per_day=
                carnivore_max_growth_per_day*
                std::clamp(1.0-carnivore_capacity_ratio,0.0,1.0);
            const double carnivore_maintenance=
                carnivore_carbon_before*
                carnivore_maintenance_per_day*
                ctx.dt_days;
            const double carnivore_growth_budget=
                carnivore_carbon_before*
                (
                    carnivore_mortality_per_day+
                    carnivore_net_growth_per_day
                )*
                ctx.dt_days;
            const double pred_demand=
                (carnivore_maintenance+carnivore_growth_budget)/
                carnivore_assimilation;
            const double killed=std::min(
                pred_demand,
                prey_biomass*std::clamp(
                    0.003*ctx.dt_days,0.0,1.0
                )
            );
            if (prey_biomass>0.0 && killed>0.0) {
                const double survival=std::clamp(
                    1.0-killed/prey_biomass,
                    0.0,
                    1.0
                );
                for (Cohort* herbivore:herbivores)
                    herbivore->count*=survival;
            }

            const double carnivore_assimilated=
                killed*carnivore_assimilation;
            const double carnivore_respired=std::min(
                carnivore_carbon_before+carnivore_assimilated,
                carnivore_maintenance
            );
            const double carnivore_after_metabolism=std::max(
                0.0,
                carnivore_carbon_before+
                    carnivore_assimilated-
                    carnivore_respired
            );
            const double carnivore_background_mortality=
                carnivore_after_metabolism*
                (-std::expm1(
                    -carnivore_mortality_per_day*ctx.dt_days
                ));
            const double carnivore_after_background=std::max(
                0.0,
                carnivore_after_metabolism-
                    carnivore_background_mortality
            );
            const double carnivore_excess_fraction=
                carnivore_after_background>0.0
                    ? std::clamp(
                        (carnivore_after_background-carnivore_capacity)/
                            carnivore_after_background,
                        0.0,
                        1.0
                    )
                    : 0.0;
            const double carnivore_density_mortality=
                carnivore_after_background*
                (-std::expm1(
                    -density_regulation_per_day*
                    carnivore_excess_fraction*
                    ctx.dt_days
                ));
            const double carnivore_carbon_after=std::max(
                0.0,
                carnivore_after_background-
                    carnivore_density_mortality
            );
            fs.add(
                cell,
                litter_,
                killed-carnivore_assimilated+
                    carnivore_background_mortality+
                    carnivore_density_mortality
            );
            fs.add(cell,respired_,carnivore_respired);
            fs.add(
                cell,respiration_rate_,
                carnivore_respired/ctx.dt_days
            );
            for (Cohort* carnivore:carnivores) {
                const double before=carnivore->count;
                if (carnivore_carbon_before>0.0)
                    carnivore->count=std::max(
                        0.0,
                        carnivore->count*
                            carnivore_carbon_after/
                            carnivore_carbon_before
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
    FieldId carbon_,litter_,respired_,respiration_rate_;
    bool fire_enabled_{};
};

class CarbonCycleSystem final : public ISimSystem {
public:
    CarbonCycleSystem(
        const FieldRegistry& r,
        bool fire_enabled,
        bool fauna_enabled
    ):
        npp_(require_field(r,"ecology.npp_kg_day")),
        heterotrophic_(require_field(
            r,"ecology.heterotrophic_respiration_kg_day"
        )),
        fire_emission_(require_field(
            r,"ecology.fire_emission_kg_day"
        )),
        fauna_respiration_(require_field(
            r,"ecology.fauna_respiration_kg_day"
        )),
        fire_enabled_(fire_enabled),
        fauna_enabled_(fauna_enabled) {}

    std::string_view id() const override {
        return "ecology.carbon_cycle";
    }
    Tick cadence_ticks() const override { return 24; }
    std::vector<std::string> after() const override {
        if (fauna_enabled_) return {"ecology.fauna"};
        if (fire_enabled_) return {"ecology.fire"};
        return {"ecology.vegetation"};
    }
    SystemAccess access() const override {
        return {
            {
                "field:ecology.npp_kg_day",
                "field:ecology.heterotrophic_respiration_kg_day",
                "field:ecology.fire_emission_kg_day",
                "field:ecology.fauna_respiration_kg_day",
                "store:climate.state"
            },
            {
                "store:climate.state",
                "field:climate.atmospheric_co2_ppm",
                "field:climate.co2_radiative_forcing_w_m2"
            }
        };
    }
    void step(SystemContext& ctx) override {
        const auto& fs=ctx.world.stores().get<FieldStore>();
        const auto sum=[&](FieldId field) {
            return std::accumulate(
                fs.column(field).begin(),
                fs.column(field).end(),
                0.0
            );
        };
        const double terrestrial_to_atmosphere_kg=
            (
                sum(heterotrophic_)+
                sum(fire_emission_)+
                sum(fauna_respiration_)-
                sum(npp_)
            )*ctx.dt_days;
        auto& climate=ctx.world.stores().get<ClimateStore>();
        climate.advance_carbon(
            terrestrial_to_atmosphere_kg,
            ctx.dt_days
        );
        climate.project_carbon(ctx.world,ctx.fields);
    }

private:
    FieldId npp_,heterotrophic_,fire_emission_,fauna_respiration_;
    bool fire_enabled_{};
    bool fauna_enabled_{};
};

} // namespace

void GeographyModule::register_fields(FieldRegistry& r) {
    r.register_field({"geography.elevation_m","m",FieldSemantics::Intensive,0.0,-11000.0,9000.0});
    r.register_field({"geography.land_fraction","1",FieldSemantics::Intensive,0.5,0.0,1.0});
    r.register_field({"geology.crust_thickness_m","m",FieldSemantics::Intensive,35'000.0,3'000.0,70'000.0});
    r.register_field({"geology.crust_density_kg_m3","kg/m3",FieldSemantics::Intensive,2'850.0,2'500.0,3'300.0,
        require_field(r,"geology.crust_thickness_m")});
    r.register_field({"geology.continental_fraction","1",FieldSemantics::Intensive,0.5,0.0,1.0});
    r.register_field({"geology.lithosphere_age_ma","Ma",FieldSemantics::Intensive,100.0,0.0,4'500.0});
    r.register_field({"geology.sediment_mass_kg","kg",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"geology.regolith_thickness_m","m",FieldSemantics::Intensive,0.0,0.0,100.0});
    r.register_field({"geology.erosion_rate_m_yr","m/yr",FieldSemantics::Intensive,0.0,0.0,0.1});
    r.register_field({"geology.trench_forcing","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"geology.volcanic_arc_forcing","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"geology.collision_forcing","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"geology.rift_forcing","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"geology.drainage_area_m2","m2",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"geology.drainage_discharge_m3_day","m3/day",FieldSemantics::Extensive,0.0,0.0,1.0e30});
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

void EcologyModule::register_fields(FieldRegistry& r) {
    r.register_field({"ecology.soil_fertility","1",FieldSemantics::Intensive,0.25,0.0,1.0});
    r.register_field({"ecology.litter_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.soil_fast_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.soil_slow_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.soil_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.heterotrophic_respiration_kg_day","kgC/day",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.soil_respired_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.grass_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.shrub_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.tree_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.vegetation_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.npp_kg_day","kgC/day",FieldSemantics::Extensive,0.0,-1.0e30,1.0e30});
    r.register_field({"ecology.fauna_respired_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.fauna_respiration_kg_day","kgC/day",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.fire_active_area_m2","m2",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.fire_active_fraction","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"ecology.fire_danger","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"ecology.fire_burned_fraction","1",FieldSemantics::Intensive,0.0,0.0,1.0});
    r.register_field({"ecology.fire_burned_area_m2","m2",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.fire_emitted_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.fire_emission_kg_day","kgC/day",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.pyrogenic_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
}
void EcologyModule::register_stores(StateStoreRegistry& stores, const FieldRegistry&) { stores.emplace<CohortStore>(); }
void EcologyModule::register_systems(Scheduler& s, const FieldRegistry& r) {
    s.add(std::make_unique<SoilSystem>(r));
    s.add(std::make_unique<VegetationSystem>(r));
    if (config_.enable_fire)
        s.add(std::make_unique<FireSystem>(r));
    if (config_.enable_fauna)
        s.add(std::make_unique<FaunaSystem>(r,config_.enable_fire));
    s.add(std::make_unique<CarbonCycleSystem>(
        r,config_.enable_fire,config_.enable_fauna
    ));
}
void EcologyModule::initialize(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    auto& cs=world.stores().get<CohortStore>();
    const auto land=require_field(r,"geography.land_fraction");
    const auto temp=require_field(r,"climate.surface_temperature_k");
    const auto regolith=require_field(r,"geology.regolith_thickness_m");
    const auto fertility=require_field(r,"ecology.soil_fertility");
    const auto litter=require_field(r,"ecology.litter_carbon_kg");
    const auto fast_carbon=require_field(
        r,"ecology.soil_fast_carbon_kg"
    );
    const auto slow_carbon=require_field(
        r,"ecology.soil_slow_carbon_kg"
    );
    const auto soil_carbon=require_field(r,"ecology.soil_carbon_kg");
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
        // Warm-start near the undisturbed long-run biomass scale. The former
        // 4 kgC/m2 multiplier forced every new world through a roughly 75%
        // artificial drawdown before approaching its own attractor.
        constexpr double initial_biomass_density_kg_m2=1.0;
        const double initial_carbon=
            effective*initial_biomass_density_kg_m2*suitability*
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
        const double soil_suitability=
            substrate_factor*(0.25+0.75*suitability);
        const double initial_fast_carbon=
            effective*0.55*soil_suitability;
        const double initial_slow_carbon=
            effective*3.50*soil_suitability;
        fs.set(c,fast_carbon,initial_fast_carbon);
        fs.set(c,slow_carbon,initial_slow_carbon);
        fs.set(
            c,
            soil_carbon,
            initial_fast_carbon+initial_slow_carbon
        );

        if (lf>0.2 && suitability>0.12 && soil_fertility>0.08) {
            constexpr double herbivore_body_mass_kg=35.0;
            constexpr double herbivore_reserve_kg=2.0;
            constexpr double carnivore_body_mass_kg=70.0;
            constexpr double carnivore_reserve_kg=4.0;
            const double weighted_forage=
                fs.get(c,pft[0])+0.55*fs.get(c,pft[1])+
                0.12*fs.get(c,pft[2]);
            const double herbivore_carbon=
                weighted_forage*
                kHerbivoreCarbonPerWeightedForage*
                kInitialHerbivoreCapacityFraction;
            const double herbivore_individual_carbon=
                (
                    herbivore_body_mass_kg+
                    herbivore_reserve_kg
                )*kFaunaCarbonFractionOfWetMass;
            const double carnivore_carbon=
                herbivore_carbon*
                kCarnivoreCarbonPerHerbivoreCarbon;
            const double carnivore_individual_carbon=
                (
                    carnivore_body_mass_kg+
                    carnivore_reserve_kg
                )*kFaunaCarbonFractionOfWetMass;
            cs.add({
                0,0,c,1,1,
                herbivore_carbon/herbivore_individual_carbon,
                herbivore_body_mass_kg,herbivore_reserve_kg
            });
            cs.add({
                0,0,c,2,2,
                carnivore_carbon/carnivore_individual_carbon,
                carnivore_body_mass_kg,carnivore_reserve_kg
            });
        }
    }
}

void EcologyModule::on_spatial_cover_changed(
    WorldState& world,
    const FieldRegistry& r
) {
    auto& fs=world.stores().get<FieldStore>();
    const FieldId land=require_field(r,"geography.land_fraction");
    const FieldId active_area=require_field(
        r,"ecology.fire_active_area_m2"
    );
    const FieldId active_fraction=require_field(
        r,"ecology.fire_active_fraction"
    );
    const FieldId fast_carbon=require_field(
        r,"ecology.soil_fast_carbon_kg"
    );
    const FieldId slow_carbon=require_field(
        r,"ecology.soil_slow_carbon_kg"
    );
    const FieldId soil_carbon=require_field(
        r,"ecology.soil_carbon_kg"
    );
    for (CellId cell:world.active_cells()) {
        const double land_area=
            world.topology().area_m2(cell)*fs.get(cell,land);
        fs.set(
            cell,
            active_fraction,
            land_area>1.0
                ? std::clamp(fs.get(cell,active_area)/land_area,0.0,1.0)
                : 0.0
        );
        fs.set(
            cell,
            soil_carbon,
            fs.get(cell,fast_carbon)+fs.get(cell,slow_carbon)
        );
    }
}

} // namespace worldsim

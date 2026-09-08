#include "worldsim/climate.hpp"

#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace worldsim {
namespace {

constexpr double seconds_per_day=86'400.0;
constexpr double maximum_substep_days=0.125;
constexpr double solar_constant_w_m2=1'361.0;
constexpr double obliquity_rad=23.44*kPi/180.0;
constexpr double orbital_days=365.2422;
constexpr double land_heat_capacity_j_m2_k=8.4e6;   // about 2 m water equivalent
constexpr double ocean_heat_capacity_j_m2_k=2.05e8; // about 50 m mixed layer
constexpr double land_albedo=0.28;
constexpr double ocean_albedo=0.30;
constexpr double outgoing_a_w_m2=210.0;
constexpr double outgoing_b_w_m2_k=2.0;
constexpr double horizontal_heat_relaxation_days=20.0;
constexpr double land_ocean_exchange_days=30.0;
constexpr double moisture_diffusivity_m2_s=2.0e5;
constexpr double lapse_rate_k_m=0.0065;
constexpr double heat_reference_temperature_k=273.15;

FieldId required_field(const FieldRegistry& r, std::string_view key) {
    const auto id=r.find(key);
    if (!id)
        throw std::runtime_error("climate requires field: "+std::string(key));
    return *id;
}

void require_nonnegative_finite(double value, std::string_view name) {
    if (!std::isfinite(value) || value<0.0 || value>1.0e30)
        throw std::runtime_error("invalid climate "+std::string(name));
}

double daily_mean_insolation(double latitude, double day) {
    const double phase=2.0*kPi*(day-80.0)/orbital_days;
    const double declination=obliquity_rad*std::sin(phase);
    const double argument=-std::tan(latitude)*std::tan(declination);
    double sunset_angle=0.0;
    if (argument<=-1.0)
        sunset_angle=kPi;
    else if (argument<1.0)
        sunset_angle=std::acos(argument);
    const double result=solar_constant_w_m2/kPi*(
        sunset_angle*std::sin(latitude)*std::sin(declination)+
        std::cos(latitude)*std::cos(declination)*std::sin(sunset_angle)
    );
    return std::max(0.0,result);
}

double saturation_water_depth_m(double temperature_k) {
    return std::clamp(
        0.028*std::exp(0.065*(temperature_k-288.0)),
        0.0015,
        0.12
    );
}

std::pair<Vec3d,Vec3d> east_north_basis(Vec3d position) {
    Vec3d east=normalized(Vec3d{-position.y,position.x,0.0});
    if (std::abs(position.x)+std::abs(position.y)<1.0e-12)
        east={0.0,1.0,0.0};
    const Vec3d north=normalized(cross(position,east));
    return {east,north};
}

std::pair<double,double> prescribed_wind(
    Vec3d position,
    double day
) {
    const double latitude=std::asin(std::clamp(position.z,-1.0,1.0));
    const double longitude=std::atan2(position.y,position.x);
    const double degrees=std::abs(latitude)*180.0/kPi;
    const double westerly_belt=
        14.0*std::exp(-std::pow((degrees-45.0)/18.0,2.0));
    const double polar_easterly=
        3.0*std::exp(-std::pow((degrees-78.0)/10.0,2.0));
    const double seasonal=2.0*kPi*day/orbital_days;
    const double east=-5.0+westerly_belt-polar_easterly+
        1.5*std::sin(3.0*longitude+seasonal)*std::cos(latitude);
    const double north=
        0.9*std::sin(2.0*latitude)*std::cos(seasonal)+
        0.7*std::sin(2.0*longitude-seasonal)*std::cos(latitude);
    return {east,north};
}

double surface_temperature(const ClimateNode& node) {
    const double land_fraction=node.area_m2>0.0
        ? std::clamp(node.land_area_m2/node.area_m2,0.0,1.0)
        : 0.0;
    return land_fraction*node.land_temperature_k+
        (1.0-land_fraction)*node.ocean_temperature_k+
        node.weather_anomaly_k;
}

double total_capacity(const ClimateNode& node) {
    const double ocean_area=std::max(0.0,node.area_m2-node.land_area_m2);
    return node.land_area_m2*land_heat_capacity_j_m2_k+
        ocean_area*ocean_heat_capacity_j_m2_k;
}

double capacity_weighted_temperature(const ClimateNode& node) {
    const double ocean_area=std::max(0.0,node.area_m2-node.land_area_m2);
    const double land_capacity=node.land_area_m2*land_heat_capacity_j_m2_k;
    const double ocean_capacity=ocean_area*ocean_heat_capacity_j_m2_k;
    const double capacity=land_capacity+ocean_capacity;
    if (!(capacity>0.0)) return 288.0;
    return (
        land_capacity*node.land_temperature_k+
        ocean_capacity*node.ocean_temperature_k
    )/capacity;
}

double shared_edge_length(
    const CubeSphereTopology& topology,
    CellId a,
    CellId b
) {
    const auto ac=topology.corners_unit(a);
    const auto bc=topology.corners_unit(b);
    std::array<Vec3d,2> shared{};
    std::size_t count=0;
    for (Vec3d x:ac) {
        for (Vec3d y:bc) {
            if (norm(x-y)<1.0e-10) {
                if (count<shared.size()) shared[count]=x;
                ++count;
                break;
            }
        }
    }
    if (count==2U) {
        return kEarthRadiusM*std::acos(std::clamp(
            dot(shared[0],shared[1]),-1.0,1.0
        ));
    }
    // The fallback remains an explicit uniform-grid geometry approximation;
    // it is never an adaptive-cover area weight interpreted as a face length.
    return 0.5*(
        std::sqrt(topology.area_m2(a))+
        std::sqrt(topology.area_m2(b))
    );
}

class ClimateSystem final : public ISimSystem {
public:
    explicit ClimateSystem(const FieldRegistry& r):
        has_magic_(r.find("magic.temperature_anomaly_k").has_value()) {}

    std::string_view id() const override { return "climate.surface"; }
    std::vector<std::string> after() const override {
        return has_magic_ ? std::vector<std::string>{"magic.flux"}
                          : std::vector<std::string>{};
    }
    SystemAccess access() const override {
        std::vector<std::string> reads{
            "field:geography.elevation_m",
            "field:geography.land_fraction",
            "store:climate.state"
        };
        if (has_magic_)
            reads.push_back("field:magic.temperature_anomaly_k");
        return {
            std::move(reads),
            {
                "store:climate.state",
                "field:climate.surface_temperature_k",
                "field:climate.precipitation_mm_day",
                "field:climate.solar_flux_w_m2",
                "field:climate.weather_anomaly_k",
                "field:climate.atmospheric_water_m3",
                "field:climate.relative_humidity",
                "field:climate.land_temperature_k",
                "field:climate.ocean_temperature_k",
                "field:climate.wind_east_m_s",
                "field:climate.wind_north_m_s",
                "field:climate.evaporation_mm_day",
                "field:climate.net_radiation_w_m2"
            }
        };
    }
    void step(SystemContext& ctx) override {
        ctx.world.stores().get<ClimateStore>().advance(
            ctx.world,ctx.fields,ctx.dt_days
        );
    }
private:
    bool has_magic_{};
};

class ClimateSurfaceExchangeSystem final : public ISimSystem {
public:
    explicit ClimateSurfaceExchangeSystem(const FieldRegistry& r):
        has_hydrology_(
            r.find("hydrology.evaporation_to_atmosphere_m3_day").has_value()
        ) {}

    std::string_view id() const override {
        return "climate.surface_exchange";
    }
    std::vector<std::string> after() const override {
        return has_hydrology_
            ? std::vector<std::string>{"hydrology.balance"}
            : std::vector<std::string>{"climate.surface"};
    }
    SystemAccess access() const override {
        std::vector<std::string> reads{
            "field:geography.land_fraction",
            "store:climate.state"
        };
        std::vector<std::string> writes{
            "store:climate.state",
            "field:climate.atmospheric_water_m3",
            "field:climate.relative_humidity",
            "field:climate.evaporation_mm_day"
        };
        if (has_hydrology_) {
            reads.push_back(
                "field:hydrology.evaporation_to_atmosphere_m3_day"
            );
            reads.push_back("store:hydrology.basins");
            writes.push_back("store:hydrology.basins");
        }
        return {std::move(reads),std::move(writes)};
    }
    void step(SystemContext& ctx) override {
        ctx.world.stores().get<ClimateStore>().exchange_surface(
            ctx.world,ctx.fields,ctx.dt_days
        );
    }
private:
    bool has_hydrology_{};
};

} // namespace

void ClimateStore::on_add_cell(CellId cell) {
    if (!has_level_) {
        reference_level_=cell.level();
        has_level_=true;
    }
}

void ClimateStore::initialize(WorldState& world, const FieldRegistry& r) {
    if (!has_level_)
        throw std::runtime_error("climate reference level is not initialized");
    const auto& fields=world.stores().get<FieldStore>();
    const FieldId elevation=required_field(r,"geography.elevation_m");
    const FieldId land=required_field(r,"geography.land_fraction");
    nodes_.clear();
    nodes_.reserve(world.active_cells().size());
    ocean_water_m3_=0.0;
    for (CellId cell:world.active_cells()) {
        if (cell.level()!=reference_level_)
            throw std::runtime_error(
                "climate initialization requires a uniform cover"
            );
        const double area=world.topology().area_m2(cell);
        const double land_area=area*fields.get(cell,land);
        const auto [latitude,longitude]=world.topology().lat_lon_rad(cell);
        (void)longitude;
        const double elevation_m=fields.get(cell,elevation);
        const double base_temperature=std::clamp(
            301.0-
            42.0*std::pow(std::abs(std::sin(latitude)),1.25)-
            std::max(0.0,elevation_m)*lapse_rate_k_m,
            175.0,
            335.0
        );
        ClimateNode node;
        node.cell=cell;
        node.area_m2=area;
        node.land_area_m2=land_area;
        node.mean_elevation_m=elevation_m;
        node.land_temperature_k=base_temperature;
        node.ocean_temperature_k=base_temperature;
        node.atmospheric_water_m3=
            area*saturation_water_depth_m(base_temperature)*0.65;
        nodes_.push_back(node);
        ocean_water_m3_+=(area-land_area)*3'700.0;
    }
    budget_={};
    rebuild_graph();
    update_diagnostics(0.0);
    project(world,r);
}

std::size_t ClimateStore::node_index(CellId cell) const {
    while (cell.level()>reference_level_) cell=cell.parent();
    const auto it=index_.find(cell);
    if (it==index_.end())
        throw std::invalid_argument("cell outside climate reference cover");
    return it->second;
}

void ClimateStore::rebuild_graph() {
    index_.clear();
    links_.clear();
    CubeSphereTopology topology;
    for (std::size_t i=0;i<nodes_.size();++i) {
        if (!index_.emplace(nodes_[i].cell,i).second)
            throw std::runtime_error("duplicate climate reference cell");
    }
    for (std::size_t i=0;i<nodes_.size();++i) {
        const Vec3d a=topology.center_unit(nodes_[i].cell);
        for (CellId neighbor:topology.neighbors4(nodes_[i].cell)) {
            const auto found=index_.find(neighbor);
            if (found==index_.end())
                throw std::runtime_error("incomplete climate reference cover");
            const std::size_t j=found->second;
            if (i>=j) continue;
            const Vec3d b=topology.center_unit(neighbor);
            const double cosine=std::clamp(dot(a,b),-1.0,1.0);
            const double distance=kEarthRadiusM*std::acos(cosine);
            const Vec3d tangent_a=normalized(b-a*cosine);
            const Vec3d tangent_b=normalized(a-b*cosine);
            links_.push_back({
                i,
                j,
                distance,
                shared_edge_length(topology,nodes_[i].cell,neighbor),
                tangent_a,
                tangent_b
            });
        }
    }
}

void ClimateStore::update_diagnostics(double day) {
    CubeSphereTopology topology;
    for (auto& node:nodes_) {
        const Vec3d position=topology.center_unit(node.cell);
        const double latitude=std::asin(
            std::clamp(position.z,-1.0,1.0)
        );
        node.solar_flux_w_m2=daily_mean_insolation(latitude,day);
        const auto [east,north]=prescribed_wind(position,day);
        node.east_wind_m_s=east;
        node.north_wind_m_s=north;
        const double capacity=node.area_m2*saturation_water_depth_m(
            surface_temperature(node)
        );
        node.relative_humidity=std::clamp(
            node.atmospheric_water_m3/std::max(1.0,capacity),0.0,2.0
        );
    }
}

void ClimateStore::advance_energy(double dt_days) {
    const double seconds=dt_days*seconds_per_day;
    for (auto& node:nodes_) {
        const double ocean_area=std::max(
            0.0,node.area_m2-node.land_area_m2
        );
        double net_energy=0.0;
        if (node.land_area_m2>0.0) {
            const double outgoing=std::max(
                0.0,
                outgoing_a_w_m2+
                outgoing_b_w_m2_k*(node.land_temperature_k-273.15)
            );
            const double absorbed=node.solar_flux_w_m2*(1.0-land_albedo);
            const double net=absorbed-outgoing;
            node.land_temperature_k+=
                net*seconds/land_heat_capacity_j_m2_k;
            net_energy+=net*node.land_area_m2;
            budget_.absorbed_solar_j+=
                absorbed*node.land_area_m2*seconds;
            budget_.outgoing_longwave_j+=
                outgoing*node.land_area_m2*seconds;
        }
        if (ocean_area>0.0) {
            const double outgoing=std::max(
                0.0,
                outgoing_a_w_m2+
                outgoing_b_w_m2_k*(node.ocean_temperature_k-273.15)
            );
            const double absorbed=node.solar_flux_w_m2*(1.0-ocean_albedo);
            const double net=absorbed-outgoing;
            node.ocean_temperature_k+=
                net*seconds/ocean_heat_capacity_j_m2_k;
            net_energy+=net*ocean_area;
            budget_.absorbed_solar_j+=absorbed*ocean_area*seconds;
            budget_.outgoing_longwave_j+=outgoing*ocean_area*seconds;
        }
        node.net_radiation_w_m2=net_energy/node.area_m2;

        if (node.land_area_m2>0.0 && ocean_area>0.0) {
            const double land_capacity=
                node.land_area_m2*land_heat_capacity_j_m2_k;
            const double ocean_capacity=
                ocean_area*ocean_heat_capacity_j_m2_k;
            const double equilibrium_transfer=(
                node.land_temperature_k-node.ocean_temperature_k
            )/(1.0/land_capacity+1.0/ocean_capacity);
            const double transfer=equilibrium_transfer*(
                -std::expm1(-dt_days/land_ocean_exchange_days)
            );
            node.land_temperature_k-=transfer/land_capacity;
            node.ocean_temperature_k+=transfer/ocean_capacity;
        }
    }

    std::vector<double> energy_delta(nodes_.size());
    const double relaxation=-std::expm1(
        -dt_days/horizontal_heat_relaxation_days
    );
    for (const Link& link:links_) {
        const double capacity_a=total_capacity(nodes_[link.a]);
        const double capacity_b=total_capacity(nodes_[link.b]);
        if (!(capacity_a>0.0) || !(capacity_b>0.0)) continue;
        const double temperature_a=
            capacity_weighted_temperature(nodes_[link.a]);
        const double temperature_b=
            capacity_weighted_temperature(nodes_[link.b]);
        const double equilibrium_transfer=(temperature_a-temperature_b)/(
            1.0/capacity_a+1.0/capacity_b
        );
        const double transfer=equilibrium_transfer*relaxation;
        energy_delta[link.a]-=transfer;
        energy_delta[link.b]+=transfer;
    }
    for (std::size_t i=0;i<nodes_.size();++i) {
        const double capacity=total_capacity(nodes_[i]);
        if (!(capacity>0.0)) continue;
        const double delta=energy_delta[i]/capacity;
        if (nodes_[i].land_area_m2>0.0)
            nodes_[i].land_temperature_k+=delta;
        if (nodes_[i].area_m2-nodes_[i].land_area_m2>0.0)
            nodes_[i].ocean_temperature_k+=delta;
        if (
            !std::isfinite(nodes_[i].land_temperature_k) ||
            !std::isfinite(nodes_[i].ocean_temperature_k) ||
            nodes_[i].land_temperature_k<150.0 ||
            nodes_[i].land_temperature_k>360.0 ||
            nodes_[i].ocean_temperature_k<150.0 ||
            nodes_[i].ocean_temperature_k>360.0
        ) {
            throw std::runtime_error(
                "climate temperature left numeric bounds at cell "+
                std::to_string(nodes_[i].cell.raw())+
                ": land="+std::to_string(nodes_[i].land_temperature_k)+
                " ocean="+std::to_string(nodes_[i].ocean_temperature_k)
            );
        }
    }
}

void ClimateStore::advance_moisture(double dt_days) {
    CubeSphereTopology topology;
    std::vector<Vec3d> winds(nodes_.size());
    for (std::size_t i=0;i<nodes_.size();++i) {
        const Vec3d position=topology.center_unit(nodes_[i].cell);
        const auto [east,north]=east_north_basis(position);
        winds[i]=east*nodes_[i].east_wind_m_s+
            north*nodes_[i].north_wind_m_s;
    }

    std::vector<double> requested(links_.size());
    std::vector<double> outgoing(nodes_.size());
    const double seconds=dt_days*seconds_per_day;
    for (std::size_t k=0;k<links_.size();++k) {
        const Link& link=links_[k];
        const double velocity=0.5*(
            dot(winds[link.a],link.tangent_a_to_b)-
            dot(winds[link.b],link.tangent_b_to_a)
        );
        const double density_a=
            nodes_[link.a].atmospheric_water_m3/nodes_[link.a].area_m2;
        const double density_b=
            nodes_[link.b].atmospheric_water_m3/nodes_[link.b].area_m2;
        const double advective=velocity>=0.0
            ? velocity*link.interface_m*seconds*density_a
            : velocity*link.interface_m*seconds*density_b;
        const double diffusive=
            moisture_diffusivity_m2_s*
            (density_a-density_b)/link.distance_m*
            link.interface_m*seconds;
        const double transfer=advective+diffusive;
        requested[k]=transfer;
        if (transfer>0.0)
            outgoing[link.a]+=transfer;
        else
            outgoing[link.b]-=transfer;
    }

    std::vector<double> delta(nodes_.size());
    std::vector<double> orographic(nodes_.size());
    for (std::size_t k=0;k<links_.size();++k) {
        const Link& link=links_[k];
        double transfer=requested[k];
        const std::size_t donor=transfer>=0.0 ? link.a : link.b;
        const std::size_t receiver=transfer>=0.0 ? link.b : link.a;
        const double scale=outgoing[donor]>
                nodes_[donor].atmospheric_water_m3
            ? nodes_[donor].atmospheric_water_m3/outgoing[donor]
            : 1.0;
        transfer*=scale;
        delta[link.a]-=transfer;
        delta[link.b]+=transfer;
        const double actual=std::abs(transfer);
        const double climb=std::max(
            0.0,
            nodes_[receiver].mean_elevation_m-
            nodes_[donor].mean_elevation_m
        );
        orographic[receiver]+=
            actual*0.35*std::clamp(climb/1'500.0,0.0,1.0);
    }
    for (std::size_t i=0;i<nodes_.size();++i) {
        nodes_[i].atmospheric_water_m3+=delta[i];
        if (
            nodes_[i].atmospheric_water_m3<0.0 &&
            nodes_[i].atmospheric_water_m3>-1.0
        ) {
            nodes_[i].atmospheric_water_m3=0.0;
        }
        require_nonnegative_finite(
            nodes_[i].atmospheric_water_m3,"atmospheric water"
        );
    }

    for (std::size_t i=0;i<nodes_.size();++i) {
        ClimateNode& node=nodes_[i];
        const double capacity=node.area_m2*saturation_water_depth_m(
            surface_temperature(node)
        );
        const double humidity=node.atmospheric_water_m3/
            std::max(1.0,capacity);
        const double supersaturation=std::max(
            0.0,node.atmospheric_water_m3-0.95*capacity
        );
        const double background=node.atmospheric_water_m3*(
            -std::expm1(
                -0.055*
                std::pow(std::clamp(humidity,0.0,1.5),4.0)*
                dt_days
            )
        );
        const double precipitation=std::min(
            node.atmospheric_water_m3,
            supersaturation+background+orographic[i]
        );
        node.atmospheric_water_m3-=precipitation;
        node.precipitation_m3_day+=precipitation;
    }
}

void ClimateStore::advance(
    WorldState& world,
    const FieldRegistry& r,
    double dt_days
) {
    require_nonnegative_finite(dt_days,"time interval");
    if (dt_days>366.0)
        throw std::invalid_argument("climate interval exceeds 366 days");
    if (!(dt_days>0.0)) return;
    for (auto& node:nodes_) {
        node.precipitation_m3_day=0.0;
        node.evaporation_m3_day=0.0;
    }

    double remaining=dt_days;
    double elapsed=0.0;
    while (remaining>0.0) {
        const double substep=std::min(remaining,maximum_substep_days);
        const double day=static_cast<double>(world.tick())*dt_days+elapsed;
        update_diagnostics(day);
        advance_energy(substep);
        advance_moisture(substep);
        remaining-=substep;
        elapsed+=substep;
    }
    for (auto& node:nodes_)
        node.precipitation_m3_day/=dt_days;

    const double weather_decay=std::exp(-dt_days/2.0);
    for (auto& node:nodes_) {
        const double random=deterministic_unit(
            world.seed(),
            fnv1a64("climate.weather.v2"),
            world.tick(),
            node.cell.raw()
        );
        node.weather_anomaly_k=
            node.weather_anomaly_k*weather_decay+
            (2.0*random-1.0)*1.5*(1.0-weather_decay);
    }
    update_diagnostics(static_cast<double>(world.tick()+1U)*dt_days);
    project(world,r);
}

void ClimateStore::exchange_surface(
    WorldState& world,
    const FieldRegistry& r,
    double dt_days
) {
    require_nonnegative_finite(dt_days,"surface exchange interval");
    if (dt_days>366.0)
        throw std::invalid_argument(
            "climate surface exchange interval exceeds 366 days"
        );
    if (!(dt_days>0.0)) return;
    auto& fields=world.stores().get<FieldStore>();
    const auto hydrology_evaporation=
        r.find("hydrology.evaporation_to_atmosphere_m3_day");
    std::vector<double> land_evaporation(nodes_.size());
    std::vector<double> land_precipitation(nodes_.size());
    const FieldId land=required_field(r,"geography.land_fraction");
    if (hydrology_evaporation) {
        for (CellId cell:world.active_cells()) {
            const std::size_t slot=fields.dense_index(cell);
            const double volume=
                fields.get_dense(slot,*hydrology_evaporation)*dt_days;
            require_nonnegative_finite(volume,"land evaporation");
            const std::size_t i=node_index(cell);
            land_evaporation[i]+=volume;
            const double area_share=
                world.topology().area_m2(cell)/nodes_[i].area_m2;
            land_precipitation[i]+=
                nodes_[i].precipitation_m3_day*dt_days*area_share*
                fields.get_dense(slot,land);
        }
    } else {
        for (std::size_t i=0;i<nodes_.size();++i)
            land_precipitation[i]=nodes_[i].precipitation_m3_day*
                dt_days*nodes_[i].land_area_m2/nodes_[i].area_m2;
    }

    std::vector<double> ocean_evaporation(nodes_.size());
    double ocean_evaporation_request=0.0;
    for (std::size_t i=0;i<nodes_.size();++i) {
        ClimateNode& node=nodes_[i];
        const double ocean_area=std::max(
            0.0,node.area_m2-node.land_area_m2
        );
        const double temperature_factor=std::clamp(
            (node.ocean_temperature_k-260.0)/35.0,0.0,1.5
        );
        const double humidity_deficit=std::clamp(
            1.0-node.relative_humidity,0.0,1.0
        );
        ocean_evaporation[i]=
            0.004*temperature_factor*humidity_deficit*ocean_area*dt_days;
        ocean_evaporation_request+=ocean_evaporation[i];
    }
    const double ocean_scale=ocean_evaporation_request>ocean_water_m3_
        ? ocean_water_m3_/ocean_evaporation_request
        : 1.0;

    double ocean_precipitation=0.0;
    for (std::size_t i=0;i<nodes_.size();++i) {
        ClimateNode& node=nodes_[i];
        const double total_precipitation=
            node.precipitation_m3_day*dt_days;
        land_precipitation[i]=std::clamp(
            land_precipitation[i],0.0,total_precipitation
        );
        const double ocean_precip=
            total_precipitation-land_precipitation[i];
        ocean_precipitation+=ocean_precip;
        budget_.land_precipitation_m3+=land_precipitation[i];
        budget_.ocean_precipitation_m3+=ocean_precip;

        const double land_evap=land_evaporation[i];
        const double ocean_evap=ocean_evaporation[i]*ocean_scale;
        node.atmospheric_water_m3+=land_evap+ocean_evap;
        node.evaporation_m3_day=(land_evap+ocean_evap)/dt_days;
        budget_.land_evaporation_m3+=land_evap;
        budget_.ocean_evaporation_m3+=ocean_evap;

        if (!hydrology_evaporation) {
            // A climate-only world has no authoritative land-water recipient.
            // Return that diagnostic land precipitation to the atmosphere.
            node.atmospheric_water_m3+=land_precipitation[i];
        }
    }

    double terrestrial_return=0.0;
    if (hydrology_evaporation) {
        terrestrial_return=world.stores()
            .get<HydrologyStore>()
            .take_pending_ocean_export_m3();
    }
    ocean_water_m3_+=
        ocean_precipitation+terrestrial_return-
        ocean_evaporation_request*ocean_scale;
    require_nonnegative_finite(ocean_water_m3_,"ocean water");
    budget_.terrestrial_ocean_return_m3+=terrestrial_return;
    for (ClimateNode& node:nodes_) {
        const double capacity=node.area_m2*saturation_water_depth_m(
            surface_temperature(node)
        );
        node.relative_humidity=std::clamp(
            node.atmospheric_water_m3/std::max(1.0,capacity),0.0,2.0
        );
    }
    project_surface_exchange(world,r);
}

void ClimateStore::project_surface_exchange(
    WorldState& world,
    const FieldRegistry& r
) const {
    auto& fields=world.stores().get<FieldStore>();
    const FieldId atmospheric_water=required_field(
        r,"climate.atmospheric_water_m3"
    );
    const FieldId humidity=required_field(r,"climate.relative_humidity");
    const FieldId evaporation=required_field(
        r,"climate.evaporation_mm_day"
    );
    for (CellId cell:world.active_cells()) {
        const std::size_t slot=fields.dense_index(cell);
        const ClimateNode& node=nodes_[node_index(cell)];
        const double area_share=
            world.topology().area_m2(cell)/node.area_m2;
        fields.set_dense(
            slot,atmospheric_water,
            node.atmospheric_water_m3*area_share
        );
        fields.set_dense(slot,humidity,node.relative_humidity);
        fields.set_dense(
            slot,evaporation,
            node.evaporation_m3_day/node.area_m2*1'000.0
        );
    }
}

void ClimateStore::project(
    WorldState& world,
    const FieldRegistry& r
) const {
    auto& fields=world.stores().get<FieldStore>();
    const FieldId elevation=required_field(r,"geography.elevation_m");
    const FieldId land=required_field(r,"geography.land_fraction");
    const FieldId surface=required_field(
        r,"climate.surface_temperature_k"
    );
    const FieldId precipitation=required_field(
        r,"climate.precipitation_mm_day"
    );
    const FieldId solar=required_field(r,"climate.solar_flux_w_m2");
    const FieldId anomaly=required_field(r,"climate.weather_anomaly_k");
    const FieldId atmospheric_water=required_field(
        r,"climate.atmospheric_water_m3"
    );
    const FieldId humidity=required_field(r,"climate.relative_humidity");
    const FieldId land_temperature=required_field(
        r,"climate.land_temperature_k"
    );
    const FieldId ocean_temperature=required_field(
        r,"climate.ocean_temperature_k"
    );
    const FieldId wind_east=required_field(r,"climate.wind_east_m_s");
    const FieldId wind_north=required_field(r,"climate.wind_north_m_s");
    const FieldId evaporation=required_field(
        r,"climate.evaporation_mm_day"
    );
    const FieldId net_radiation=required_field(
        r,"climate.net_radiation_w_m2"
    );
    const auto magic=r.find("magic.temperature_anomaly_k");
    for (CellId cell:world.active_cells()) {
        const std::size_t slot=fields.dense_index(cell);
        const ClimateNode& node=nodes_[node_index(cell)];
        const double local_land=fields.get_dense(slot,land);
        const double local_elevation=fields.get_dense(slot,elevation);
        const double local_temperature=
            local_land*node.land_temperature_k+
            (1.0-local_land)*node.ocean_temperature_k-
            (local_elevation-node.mean_elevation_m)*lapse_rate_k_m+
            node.weather_anomaly_k+
            (magic ? fields.get_dense(slot,*magic) : 0.0);
        const double area_share=
            world.topology().area_m2(cell)/node.area_m2;
        fields.set_dense(
            slot,surface,std::clamp(local_temperature,150.0,360.0)
        );
        fields.set_dense(
            slot,
            precipitation,
            node.precipitation_m3_day/node.area_m2*1'000.0
        );
        fields.set_dense(slot,solar,node.solar_flux_w_m2);
        fields.set_dense(slot,anomaly,node.weather_anomaly_k);
        fields.set_dense(
            slot,atmospheric_water,
            node.atmospheric_water_m3*area_share
        );
        fields.set_dense(slot,humidity,node.relative_humidity);
        fields.set_dense(slot,land_temperature,node.land_temperature_k);
        fields.set_dense(slot,ocean_temperature,node.ocean_temperature_k);
        fields.set_dense(slot,wind_east,node.east_wind_m_s);
        fields.set_dense(slot,wind_north,node.north_wind_m_s);
        fields.set_dense(
            slot,evaporation,
            node.evaporation_m3_day/node.area_m2*1'000.0
        );
        fields.set_dense(slot,net_radiation,node.net_radiation_w_m2);
    }
}

double ClimateStore::total_atmospheric_water_m3() const {
    return std::accumulate(
        nodes_.begin(),nodes_.end(),0.0,
        [](double total, const ClimateNode& node) {
            return total+node.atmospheric_water_m3;
        }
    );
}

double ClimateStore::total_surface_heat_j() const {
    double total=0.0;
    for (const ClimateNode& node:nodes_) {
        const double ocean_area=std::max(
            0.0,node.area_m2-node.land_area_m2
        );
        total+=
            node.land_area_m2*land_heat_capacity_j_m2_k*
                (node.land_temperature_k-heat_reference_temperature_k)+
            ocean_area*ocean_heat_capacity_j_m2_k*
                (node.ocean_temperature_k-heat_reference_temperature_k);
    }
    return total;
}

void ClimateStore::save(BinaryWriter& writer) const {
    writer.pod(reference_level_);
    writer.pod(ocean_water_m3_);
    writer.pod(budget_.absorbed_solar_j);
    writer.pod(budget_.outgoing_longwave_j);
    writer.pod(budget_.land_precipitation_m3);
    writer.pod(budget_.ocean_precipitation_m3);
    writer.pod(budget_.land_evaporation_m3);
    writer.pod(budget_.ocean_evaporation_m3);
    writer.pod(budget_.terrestrial_ocean_return_m3);
    writer.pod<std::uint64_t>(static_cast<std::uint64_t>(nodes_.size()));
    for (const ClimateNode& node:nodes_) {
        writer.pod(node.cell.raw());
        writer.pod(node.area_m2);
        writer.pod(node.land_area_m2);
        writer.pod(node.mean_elevation_m);
        writer.pod(node.land_temperature_k);
        writer.pod(node.ocean_temperature_k);
        writer.pod(node.atmospheric_water_m3);
        writer.pod(node.precipitation_m3_day);
        writer.pod(node.evaporation_m3_day);
        writer.pod(node.solar_flux_w_m2);
        writer.pod(node.net_radiation_w_m2);
        writer.pod(node.relative_humidity);
        writer.pod(node.east_wind_m_s);
        writer.pod(node.north_wind_m_s);
        writer.pod(node.weather_anomaly_k);
    }
}

void ClimateStore::load(BinaryReader& reader, std::uint32_t version) {
    if (version!=1)
        throw std::runtime_error("unsupported climate state snapshot version");
    const std::uint8_t level=reader.pod<std::uint8_t>();
    if (!has_level_ || level!=reference_level_)
        throw std::runtime_error("climate reference level mismatch");
    const double ocean_water=reader.pod<double>();
    require_nonnegative_finite(ocean_water,"ocean water");
    ClimateBudget budget{
        reader.pod<double>(),
        reader.pod<double>(),
        reader.pod<double>(),
        reader.pod<double>(),
        reader.pod<double>(),
        reader.pod<double>(),
        reader.pod<double>()
    };
    require_nonnegative_finite(budget.absorbed_solar_j,"solar budget");
    require_nonnegative_finite(
        budget.outgoing_longwave_j,"longwave budget"
    );
    require_nonnegative_finite(
        budget.land_precipitation_m3,"land precipitation budget"
    );
    require_nonnegative_finite(
        budget.ocean_precipitation_m3,"ocean precipitation budget"
    );
    require_nonnegative_finite(
        budget.land_evaporation_m3,"land evaporation budget"
    );
    require_nonnegative_finite(
        budget.ocean_evaporation_m3,"ocean evaporation budget"
    );
    require_nonnegative_finite(
        budget.terrestrial_ocean_return_m3,"ocean return budget"
    );
    const std::uint64_t count=reader.pod<std::uint64_t>();
    const std::uint64_t expected=6ULL*(1ULL<<(2U*level));
    constexpr std::size_t bytes_per_node=8U+14U*8U;
    if (count!=expected || count>reader.remaining()/bytes_per_node)
        throw std::runtime_error("invalid climate node count");
    std::vector<ClimateNode> nodes;
    const CubeSphereTopology topology;
    nodes.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i=0;i<count;++i) {
        ClimateNode node;
        node.cell=CellId(reader.pod<std::uint64_t>());
        node.area_m2=reader.pod<double>();
        node.land_area_m2=reader.pod<double>();
        node.mean_elevation_m=reader.pod<double>();
        node.land_temperature_k=reader.pod<double>();
        node.ocean_temperature_k=reader.pod<double>();
        node.atmospheric_water_m3=reader.pod<double>();
        node.precipitation_m3_day=reader.pod<double>();
        node.evaporation_m3_day=reader.pod<double>();
        node.solar_flux_w_m2=reader.pod<double>();
        node.net_radiation_w_m2=reader.pod<double>();
        node.relative_humidity=reader.pod<double>();
        node.east_wind_m_s=reader.pod<double>();
        node.north_wind_m_s=reader.pod<double>();
        node.weather_anomaly_k=reader.pod<double>();
        const bool cell_is_valid=
            node.cell.valid() && node.cell.level()==level;
        const double expected_area=cell_is_valid
            ? topology.area_m2(node.cell)
            : 0.0;
        if (
            !cell_is_valid ||
            (!nodes.empty() && !(nodes.back().cell<node.cell)) ||
            !std::isfinite(node.area_m2) || !(node.area_m2>0.0) ||
            std::abs(node.area_m2-expected_area)>
                expected_area*1.0e-12 ||
            !std::isfinite(node.land_area_m2) ||
            node.land_area_m2<0.0 || node.land_area_m2>node.area_m2 ||
            !std::isfinite(node.mean_elevation_m) ||
            std::abs(node.mean_elevation_m)>1.0e6 ||
            !std::isfinite(node.land_temperature_k) ||
            node.land_temperature_k<150.0 ||
            node.land_temperature_k>360.0 ||
            !std::isfinite(node.ocean_temperature_k) ||
            node.ocean_temperature_k<150.0 ||
            node.ocean_temperature_k>360.0 ||
            !std::isfinite(node.precipitation_m3_day) ||
            node.precipitation_m3_day<0.0 ||
            node.precipitation_m3_day>node.area_m2 ||
            !std::isfinite(node.evaporation_m3_day) ||
            node.evaporation_m3_day<0.0 ||
            node.evaporation_m3_day>node.area_m2 ||
            !std::isfinite(node.solar_flux_w_m2) ||
            node.solar_flux_w_m2<0.0 || node.solar_flux_w_m2>1'500.0 ||
            !std::isfinite(node.net_radiation_w_m2) ||
            std::abs(node.net_radiation_w_m2)>2'000.0 ||
            !std::isfinite(node.relative_humidity) ||
            node.relative_humidity<0.0 || node.relative_humidity>2.0 ||
            !std::isfinite(node.east_wind_m_s) ||
            std::abs(node.east_wind_m_s)>100.0 ||
            !std::isfinite(node.north_wind_m_s) ||
            std::abs(node.north_wind_m_s)>100.0 ||
            !std::isfinite(node.weather_anomaly_k) ||
            std::abs(node.weather_anomaly_k)>30.0
        ) {
            throw std::runtime_error("invalid climate node state");
        }
        require_nonnegative_finite(
            node.atmospheric_water_m3,"atmospheric water"
        );
        nodes.push_back(node);
    }
    nodes_=std::move(nodes);
    ocean_water_m3_=ocean_water;
    budget_=budget;
    rebuild_graph();
}

void ClimateStore::validate_active_cover(
    const std::set<CellId>& active_cells
) const {
    if (nodes_.empty())
        throw std::runtime_error("empty climate reference cover");
    for (CellId cell:active_cells) (void)node_index(cell);
}

double total_planet_water_m3(
    const WorldState& world,
    const FieldRegistry& r
) {
    const ClimateStore& climate=world.stores().get<ClimateStore>();
    return climate.ocean_water_m3()+
        climate.total_atmospheric_water_m3()+
        total_land_water_m3(world,r);
}

void ClimateModule::register_fields(FieldRegistry& r) {
    r.register_field({
        "climate.surface_temperature_k","K",
        FieldSemantics::Intensive,288.0,150.0,360.0
    });
    r.register_field({
        "climate.precipitation_mm_day","mm/day",
        FieldSemantics::Intensive,0.0,0.0,1'000.0
    });
    r.register_field({
        "climate.solar_flux_w_m2","W/m2",
        FieldSemantics::Intensive,250.0,0.0,1'500.0
    });
    r.register_field({
        "climate.weather_anomaly_k","K",
        FieldSemantics::Intensive,0.0,-30.0,30.0
    });
    r.register_field({
        "climate.atmospheric_water_m3","m3",
        FieldSemantics::Extensive,0.0,0.0,1.0e30
    });
    r.register_field({
        "climate.relative_humidity","1",
        FieldSemantics::Intensive,0.65,0.0,2.0
    });
    r.register_field({
        "climate.land_temperature_k","K",
        FieldSemantics::Intensive,288.0,150.0,360.0
    });
    r.register_field({
        "climate.ocean_temperature_k","K",
        FieldSemantics::Intensive,288.0,150.0,360.0
    });
    r.register_field({
        "climate.wind_east_m_s","m/s",
        FieldSemantics::Intensive,0.0,-100.0,100.0
    });
    r.register_field({
        "climate.wind_north_m_s","m/s",
        FieldSemantics::Intensive,0.0,-100.0,100.0
    });
    r.register_field({
        "climate.evaporation_mm_day","mm/day",
        FieldSemantics::Intensive,0.0,0.0,1'000.0
    });
    r.register_field({
        "climate.net_radiation_w_m2","W/m2",
        FieldSemantics::Intensive,0.0,-2'000.0,2'000.0
    });
}

void ClimateModule::register_stores(
    StateStoreRegistry& stores,
    const FieldRegistry&
) {
    stores.emplace<ClimateStore>();
}

void ClimateModule::register_systems(
    Scheduler& scheduler,
    const FieldRegistry& r
) {
    scheduler.add(std::make_unique<ClimateSystem>(r));
    scheduler.add(std::make_unique<ClimateSurfaceExchangeSystem>(r));
}

void ClimateModule::initialize(WorldState& world, const FieldRegistry& r) {
    world.stores().get<ClimateStore>().initialize(world,r);
}

void ClimateModule::on_spatial_cover_changed(
    WorldState& world,
    const FieldRegistry& r
) {
    world.stores().get<ClimateStore>().project(world,r);
}

} // namespace worldsim

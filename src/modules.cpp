#include "worldsim/modules.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace worldsim {
namespace {

FieldId require_field(const FieldRegistry& r, std::string_view key) {
    const auto id=r.find(key);
    if (!id) throw std::runtime_error("required field missing: "+std::string(key));
    return *id;
}

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
        : temp_(require_field(r,"climate.surface_temperature_k")), precip_(require_field(r,"climate.precipitation_mm_day")),
          land_(require_field(r,"geography.land_fraction")), water_(require_field(r,"hydrology.soil_water_m3")),
          runoff_(require_field(r,"hydrology.runoff_m3_day")) {}
    std::string_view id() const override { return "ecology.hydrology"; }
    Tick cadence_ticks() const override { return 6; }
    std::vector<std::string> after() const override { return {"climate.surface"}; }
    SystemAccess access() const override {
        return {{"field:climate.surface_temperature_k","field:climate.precipitation_mm_day","field:geography.land_fraction","field:hydrology.soil_water_m3"},
                {"field:hydrology.soil_water_m3","field:hydrology.runoff_m3_day"}};
    }
    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            const double land=fs.get(cell,land_);
            const double effective_area=area*land;
            if (effective_area<=1.0) { fs.set(cell,water_,0.0); fs.set(cell,runoff_,0.0); continue; }
            double water=fs.get(cell,water_);
            const double rain=fs.get(cell,precip_)*0.001*effective_area*ctx.dt_days;
            const double temp=fs.get(cell,temp_);
            const double evap_depth=std::max(0.0,temp-258.0)*0.000035*ctx.dt_days;
            const double evap=std::min(water+rain,evap_depth*effective_area);
            water+=rain-evap;
            const double capacity=0.35*effective_area;
            const double excess=std::max(0.0,water-capacity);
            water-=excess;
            fs.set(cell,water_,water);
            fs.set(cell,runoff_,excess/std::max(ctx.dt_days,1e-12));
        }
    }
private:
    FieldId temp_,precip_,land_,water_,runoff_;
};

class VegetationSystem final : public ISimSystem {
public:
    explicit VegetationSystem(const FieldRegistry& r)
        : temp_(require_field(r,"climate.surface_temperature_k")), solar_(require_field(r,"climate.solar_flux_w_m2")),
          land_(require_field(r,"geography.land_fraction")), water_(require_field(r,"hydrology.soil_water_m3")),
          growth_(require_field(r,"magic.growth_factor")), carbon_(require_field(r,"ecology.vegetation_carbon_kg")),
          npp_(require_field(r,"ecology.npp_kg_day")) {}
    std::string_view id() const override { return "ecology.vegetation"; }
    Tick cadence_ticks() const override { return 24; }
    std::vector<std::string> after() const override { return {"ecology.hydrology","magic.flux"}; }
    SystemAccess access() const override {
        return {{"field:climate.surface_temperature_k","field:climate.solar_flux_w_m2","field:geography.land_fraction","field:hydrology.soil_water_m3","field:magic.growth_factor","field:ecology.vegetation_carbon_kg"},
                {"field:ecology.vegetation_carbon_kg","field:ecology.npp_kg_day"}};
    }
    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        for (CellId cell:ctx.world.active_cells()) {
            const double area=ctx.world.topology().area_m2(cell);
            const double land=fs.get(cell,land_);
            const double effective_area=area*land;
            double carbon=fs.get(cell,carbon_);
            if (effective_area<=1.0) { fs.set(cell,carbon_,0.0); fs.set(cell,npp_,0.0); continue; }
            const double temp=fs.get(cell,temp_);
            const double tf=std::exp(-std::pow((temp-293.0)/18.0,2.0));
            const double wf=std::clamp(fs.get(cell,water_)/(0.18*effective_area),0.0,1.0);
            const double sf=std::clamp(fs.get(cell,solar_)/340.0,0.05,1.2);
            const double potential=effective_area*0.00075*tf*wf*sf*fs.get(cell,growth_);
            const double capacity=effective_area*9.0;
            const double density_factor=std::clamp(1.0-carbon/std::max(1.0,capacity),0.0,1.0);
            const double gross=potential*density_factor;
            const double respiration=carbon*0.00035*std::pow(2.0,(temp-283.0)/10.0);
            const double net=gross-respiration;
            carbon=std::clamp(carbon+net*ctx.dt_days,0.0,capacity);
            fs.set(cell,carbon_,carbon);
            fs.set(cell,npp_,net);
        }
    }
private:
    FieldId temp_,solar_,land_,water_,growth_,carbon_,npp_;
};

class FaunaSystem final : public ISimSystem {
public:
    explicit FaunaSystem(const FieldRegistry& r): carbon_(require_field(r,"ecology.vegetation_carbon_kg")) {}
    std::string_view id() const override { return "ecology.fauna"; }
    Tick cadence_ticks() const override { return 24; }
    std::vector<std::string> after() const override { return {"ecology.vegetation"}; }
    SystemAccess access() const override {
        return {{"field:ecology.vegetation_carbon_kg","store:ecology.cohorts"},{"field:ecology.vegetation_carbon_kg","store:ecology.cohorts"}};
    }
    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        auto& cs=ctx.world.stores().get<CohortStore>();
        for (CellId cell:ctx.world.active_cells()) {
            auto cohorts=cs.in_cell(cell);
            std::vector<Cohort*> herbivores, carnivores;
            for (auto& ref:cohorts) {
                Cohort& c=ref.get();
                if (c.functional_group==1) herbivores.push_back(&c);
                else if (c.functional_group==2) carnivores.push_back(&c);
            }
            double vegetation=fs.get(cell,carbon_);
            double total_demand=0.0;
            for (const Cohort* h:herbivores) total_demand+=h->count*h->body_mass_kg*0.018*ctx.dt_days;
            const double consumed=std::min(total_demand,vegetation*0.025);
            vegetation-=consumed;
            const double food_ratio=total_demand>0.0 ? std::clamp(consumed/total_demand,0.0,1.0) : 1.0;
            for (Cohort* h:herbivores) {
                const double rate=0.0045*food_ratio-0.006*(1.0-food_ratio);
                h->count=std::max(0.0,h->count*std::exp(rate*ctx.dt_days));
            }
            double prey_biomass=0.0;
            for (const Cohort* h:herbivores) prey_biomass+=h->count*h->body_mass_kg;
            double pred_demand=0.0;
            for (const Cohort* c:carnivores) pred_demand+=c->count*c->body_mass_kg*0.025*ctx.dt_days;
            const double killed=std::min(pred_demand,prey_biomass*0.003);
            if (prey_biomass>0.0 && killed>0.0) {
                const double survival=std::clamp(1.0-killed/prey_biomass,0.0,1.0);
                for (Cohort* h:herbivores) h->count*=survival;
            }
            const double pred_food=pred_demand>0.0 ? std::clamp(killed/pred_demand,0.0,1.0) : 1.0;
            for (Cohort* c:carnivores) {
                const double before=c->count;
                const double rate=0.0035*pred_food-0.007*(1.0-pred_food);
                c->count=std::max(0.0,c->count*std::exp(rate*ctx.dt_days));
                if (before>=1.0 && c->count<1.0) ctx.world.emit({ctx.world.tick(),"cohort.near_extinction",cell,c->id,c->count});
            }
            fs.set(cell,carbon_,vegetation);
        }
    }
private:
    FieldId carbon_;
};

} // namespace

void GeographyModule::register_fields(FieldRegistry& r) {
    r.register_field({"geography.elevation_m","m",FieldSemantics::Intensive,0.0,-11000.0,9000.0});
    r.register_field({"geography.land_fraction","1",FieldSemantics::Intensive,0.5,0.0,1.0});
}

void GeographyModule::initialize(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    const auto elev=require_field(r,"geography.elevation_m"), land=require_field(r,"geography.land_fraction");
    const double p0=deterministic_unit(world.seed(),fnv1a64("geo.phase0"),0,0)*2.0*kPi;
    const double p1=deterministic_unit(world.seed(),fnv1a64("geo.phase1"),0,0)*2.0*kPi;
    const double p2=deterministic_unit(world.seed(),fnv1a64("geo.phase2"),0,0)*2.0*kPi;
    for (CellId c:world.active_cells()) {
        const Vec3d p=world.topology().center_unit(c);
        const double continental=0.75*std::sin(2.4*p.x+1.7*p.y+p0)+0.55*std::sin(3.3*p.y-2.1*p.z+p1)+0.35*std::cos(5.2*p.z+1.3*p.x+p2);
        const double mountain=std::pow(std::max(0.0,std::sin(7.0*p.x-5.0*p.y+2.0*p.z+p1)),3.0);
        const double e=1900.0*(continental-0.18)+2400.0*mountain;
        const double lf=std::clamp(0.5+e/900.0,0.0,1.0);
        fs.set(c,elev,e); fs.set(c,land,lf);
    }
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
    r.register_field({"ecology.vegetation_carbon_kg","kgC",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    r.register_field({"ecology.npp_kg_day","kgC/day",FieldSemantics::Extensive,0.0,-1.0e30,1.0e30});
}
void EcologyModule::register_stores(StateStoreRegistry& stores, const FieldRegistry&) { stores.emplace<CohortStore>(); }
void EcologyModule::register_systems(Scheduler& s, const FieldRegistry& r) {
    s.add(std::make_unique<HydrologySystem>(r));
    s.add(std::make_unique<VegetationSystem>(r));
    s.add(std::make_unique<FaunaSystem>(r));
}
void EcologyModule::initialize(WorldState& world, const FieldRegistry& r) {
    auto& fs=world.stores().get<FieldStore>();
    auto& cs=world.stores().get<CohortStore>();
    const auto land=require_field(r,"geography.land_fraction"), temp=require_field(r,"climate.surface_temperature_k");
    const auto water=require_field(r,"hydrology.soil_water_m3"), carbon=require_field(r,"ecology.vegetation_carbon_kg");
    for (CellId c:world.active_cells()) {
        const double area=world.topology().area_m2(c), lf=fs.get(c,land), effective=area*lf;
        const double suitability=std::exp(-std::pow((fs.get(c,temp)-291.0)/24.0,2.0));
        fs.set(c,water,effective*0.14);
        fs.set(c,carbon,effective*4.0*suitability);
        if (lf>0.2 && suitability>0.12) {
            const double km2=effective/1.0e6;
            cs.add({0,0,c,1,1,std::max(10.0,km2*0.7*suitability),35.0,2.0});
            cs.add({0,0,c,2,2,std::max(2.0,km2*0.015*suitability),70.0,4.0});
        }
    }
}

} // namespace worldsim

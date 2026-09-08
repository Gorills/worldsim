#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace worldsim;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void near(double a,double b,double relative,const char* message) {
    check(std::isfinite(a) && std::isfinite(b) && std::abs(a-b)<=relative*std::max({1.0,std::abs(a),std::abs(b)}),message);
}
FieldId id(const Simulation& sim,const char* key) { return *sim.fields().find(key); }
class ForcingSystem final : public ISimSystem {
public:
    std::string_view id() const override { return "climate.surface"; }
    SystemAccess access() const override { return {}; }
    void step(SystemContext&) override {}
};
class FixtureModule final : public ISimModule {
public:
    std::string_view id() const override { return "fixture"; }
    void register_fields(FieldRegistry& r) override {
        r.register_field({"geography.elevation_m","m",FieldSemantics::Intensive,100,-1e5,1e5});
        r.register_field({"geography.land_fraction","1",FieldSemantics::Intensive,1,0,1});
        r.register_field({"geology.regolith_thickness_m","m",FieldSemantics::Intensive,1,0,1e5});
        r.register_field({"climate.surface_temperature_k","K",FieldSemantics::Intensive,250,100,400});
        r.register_field({"climate.precipitation_mm_day","mm/day",FieldSemantics::Intensive,0,0,1000});
        r.register_field({"geology.drainage_discharge_m3_day","m3/day",FieldSemantics::Intensive,0,0,1e30});
        r.register_field({"geology.drainage_area_m2","m2",FieldSemantics::Intensive,0,0,1e30});
    }
    void register_systems(Scheduler& s,const FieldRegistry&) override { s.add(std::make_unique<ForcingSystem>()); }
};
std::unique_ptr<Simulation> fixture(std::uint8_t level=2) {
    auto sim=std::make_unique<Simulation>(42,SimulationConfig{level,static_cast<std::uint8_t>(level+1),3600});
    sim->add_module(std::make_unique<FixtureModule>()); sim->add_module(std::make_unique<HydrologyModule>()); sim->build();
    auto& fs=sim->world().stores().get<FieldStore>();
    for (CellId cell:sim->world().active_cells()) for (auto key:{"hydrology.soil_water_m3","hydrology.groundwater_m3"}) fs.set(cell,id(*sim,key),0);
    return sim;
}
void advance(Simulation& sim,double days) {
    Scheduler scheduler; FixtureModule().register_systems(scheduler,sim.fields()); HydrologyModule().register_systems(scheduler,sim.fields()); scheduler.finalize();
    SystemContext ctx{sim.world(),sim.fields(),days};
    for (auto* system:scheduler.order()) if (system->id()=="hydrology.balance") system->step(ctx);
}
void set_all(Simulation& sim,const char* key,double value) {
    auto& fs=sim.world().stores().get<FieldStore>();
    for (CellId cell:sim.world().active_cells()) fs.set(cell,id(sim,key),value);
}
std::vector<CellId> channel(Simulation& sim,bool lake=false) {
    auto& store=sim.world().stores().get<HydrologyStore>();
    std::vector<CellId> cells;
    const std::array<double,5> heights=lake ? std::array<double,5>{1,1.2,2,0.5,-1} : std::array<double,5>{4,3,2,1,-1};
    for (std::uint32_t x=1;x<=5;++x) {
        const CellId cell=CellId::make(0,3,x,3); cells.push_back(cell);
        store.set_bed_elevation(cell,heights[x-1]);
    }
    return cells;
}
void assert_budget(const Simulation& sim,double initial) {
    const auto& b=sim.world().stores().get<HydrologyStore>().budget();
    near(initial+b.precipitation_m3,total_land_water_m3(sim.world(),sim.fields())+b.evaporation_m3+b.ocean_export_m3,2e-11,"land water budget does not close");
}
void snow_recharge_and_recession() {
    auto sim=fixture();
    const double initial=total_land_water_m3(sim->world(),sim->fields());
    set_all(*sim,"climate.precipitation_mm_day",8);
    advance(*sim,20);
    auto& fs=sim->world().stores().get<FieldStore>();
    const auto cell=*sim->world().active_cells().begin();
    const double area=sim->world().topology().area_m2(cell);
    near(fs.get(cell,id(*sim,"hydrology.snow_water_m3")),0.16*area,1e-12,"winter precipitation was not retained as snow");
    near(sim->world().stores().get<HydrologyStore>().total_surface_m3(),0,1e-12,"frozen precipitation created river flow");
    assert_budget(*sim,initial);
    set_all(*sim,"climate.precipitation_mm_day",0);
    set_all(*sim,"climate.surface_temperature_k",280);
    advance(*sim,12);
    check(fs.get(cell,id(*sim,"hydrology.snow_water_m3"))<0.001*area,"warm period did not melt snow");
    check(fs.get(cell,id(*sim,"hydrology.groundwater_m3"))>0,"melt did not recharge groundwater");
    check(fs.get(cell,id(*sim,"hydrology.soil_water_m3"))>0,"melt did not wet root zone");
    assert_budget(*sim,initial);
    // A dry period retains baseflow while groundwater declines. Surface
    // reinfiltration is deliberately still coupled, so net storage need not
    // follow the isolated reservoir's exponential exactly.
    set_all(*sim,"climate.surface_temperature_k",250);
    for (CellId c:sim->world().active_cells()) {
        const double a=sim->world().topology().area_m2(c);
        fs.set(c,id(*sim,"hydrology.soil_water_m3"),0.65*soil_water_capacity_depth_m(1)*a);
        fs.set(c,id(*sim,"hydrology.groundwater_m3"),0.1*a);
    }
    advance(*sim,45);
    const double remaining=fs.get(cell,id(*sim,"hydrology.groundwater_m3"));
    check(remaining>0 && remaining<0.1*area,"groundwater did not decline during dry-weather recession");
    check(fs.get(cell,id(*sim,"hydrology.baseflow_m3_day"))>0,"dry weather lost groundwater-fed flow");
}
void delayed_routing_and_timestep() {
    auto a=fixture(3); const auto cells=channel(*a);
    auto& store=a->world().stores().get<HydrologyStore>();
    const double pulse=store.volume_at_level(store.node_index(cells[0]),4.1);
    store.add_surface_water(cells[0],pulse);
    store.route(0.001);
    check(store.nodes()[store.node_index(cells[1])].surface_m3>0,"pulse did not enter downstream reach");
    near(store.nodes()[store.node_index(cells[2])].surface_m3,0,1e-12,"new arrivals moved twice in one internal step");
    near(store.total_surface_m3()+store.budget().ocean_export_m3,pulse,1e-12,"routing lost water");
    auto b=fixture(3); channel(*b);
    auto& other=b->world().stores().get<HydrologyStore>();
    other.add_surface_water(cells[0],pulse); other.route(0.001);
    double peak=0; int peak_day=0;
    for (int day=1;day<=90;++day) {
        store.route(1.0);
        for (int sub=0;sub<32;++sub) other.route(1.0/32.0);
        const double q=store.nodes()[store.node_index(cells[3])].discharge_m3_day;
        if (q>peak) { peak=q; peak_day=day; }
    }
    check(peak_day>2 && peak_day<80,"downstream hydrograph lacks delayed peak");
    check(store.budget().ocean_export_m3>pulse*0.5,"pulse never reached ocean outlet");
    near(store.total_surface_m3()+store.budget().ocean_export_m3,pulse,2e-12,"outlet budget lost water");
    near(store.budget().ocean_export_m3,other.budget().ocean_export_m3,0.01,"routing fails timestep convergence");
}
void lakes_spill_connect_and_terrain_change() {
    auto sim=fixture(3); const auto cells=channel(*sim,true);
    auto& store=sim->world().stores().get<HydrologyStore>();
    const auto a=store.node_index(cells[0]),b=store.node_index(cells[1]);
    near(store.spill_level_m(a),2,1e-12,"depression outlet height is wrong");
    store.add_surface_water(cells[0],store.volume_at_level(a,1.8));
    store.add_surface_water(cells[1],store.volume_at_level(b,1.8));
    const double initial=store.total_surface_m3();
    store.route(30);
    near(store.level_m(a),1.8,1e-10,"resting lake level drifted");
    near(store.level_m(b),1.8,1e-10,"communicating lake levels differ");
    check(store.lake_id(a)!=0 && store.lake_id(a)==store.lake_id(b),"connected lake cells lack common identity");
    near(store.budget().ocean_export_m3,0,1e-12,"lake crossed a dry spill sill");
    // Lowering a physical sill must release stored water without reinitializing it.
    store.set_bed_elevation(cells[2],1.4);
    near(store.total_surface_m3(),initial,1e-12,"terrain change destroyed lake volume");
    store.route(120);
    check(store.budget().ocean_export_m3>0,"lowered sill did not release lake water");
    check(store.level_m(a)<1.8,"lake did not recede after sill breach");
    near(store.total_surface_m3()+store.budget().ocean_export_m3,initial,1e-11,"lake breach budget failed");
    // Filling above the sill in an unchanged basin must overflow as well.
    auto full=fixture(3); const auto fc=channel(*full,true);
    auto& filled=full->world().stores().get<HydrologyStore>();
    filled.add_surface_water(fc[0],filled.volume_at_level(filled.node_index(fc[0]),3.5));
    filled.route(120);
    check(filled.budget().ocean_export_m3>0,"full lake never overflowed");
}
void mixed_cover_exchange_and_snapshot() {
    auto coarse=fixture(2),fine=fixture(2);
    auto& cs=coarse->world().stores().get<HydrologyStore>();
    auto& fs=fine->world().stores().get<HydrologyStore>();
    const CellId parent=CellId::make(0,2,1,1);
    const double volume=cs.volume_at_level(cs.node_index(parent),100.8);
    cs.add_surface_water(parent,volume); fs.add_surface_water(parent,volume);
    set_all(*coarse,"climate.surface_temperature_k",280); set_all(*fine,"climate.surface_temperature_k",280);
    set_all(*coarse,"climate.precipitation_mm_day",5); set_all(*fine,"climate.precipitation_mm_day",5);
    fine->world().refine(parent);
    HydrologyModule().on_spatial_cover_changed(fine->world(),fine->fields());
    const double initial=total_land_water_m3(coarse->world(),coarse->fields());
    near(initial,total_land_water_m3(fine->world(),fine->fields()),1e-12,"LOD created water");
    advance(*coarse,0.5); advance(*fine,0.5);
    for (std::size_t i=0;i<cs.nodes().size();++i) {
        near(cs.nodes()[i].surface_m3,fs.nodes()[i].surface_m3,2e-11,"coarse/fine exchange changed surface water");
        near(cs.spill_level_m(i),fs.spill_level_m(i),1e-12,"LOD changed basin sill");
    }
    assert_budget(*fine,initial);
    // Integrated flux history must survive a partial geology window.
    const auto snapshot=fine->save_snapshot();
    auto restored=fixture(2); restored->load_snapshot(snapshot);
    check(restored->save_snapshot()==snapshot,"hydrology snapshot roundtrip differs");
    advance(*fine,1.0); advance(*restored,1.0);
    check(fine->save_snapshot()==restored->save_snapshot(),"hydrology continuation diverged");
    std::vector<double> expected_discharge;
    for (const auto& n:fine->world().stores().get<HydrologyStore>().nodes()) expected_discharge.push_back(n.erosion_volume_m3/1.5);
    fine->world().stores().get<HydrologyStore>().consume_geology_discharge(fine->world(),fine->fields());
    restored->world().stores().get<HydrologyStore>().consume_geology_discharge(restored->world(),restored->fields());
    check(fine->save_snapshot()==restored->save_snapshot(),"restored routed-volume window differs");
    for (std::size_t i=0;i<expected_discharge.size();++i) {
        double actual=0;
        const auto& graph=fine->world().stores().get<HydrologyStore>();
        for (const auto& part:fine->world().resolve_active_cover(graph.nodes()[i].cell))
            actual+=fine->world().stores().get<FieldStore>().get(part.cell,id(*fine,"geology.drainage_discharge_m3_day"));
        near(actual,expected_discharge[i],2e-12,"geology did not consume the entire routed-volume window");
        near(graph.nodes()[i].erosion_volume_m3,0,1e-12,"geology window was not consumed");
    }
    auto& fields=fine->world().stores().get<FieldStore>();
    double projected=0;
    for (double x:fields.column(id(*fine,"hydrology.surface_water_m3"))) projected+=x;
    near(projected,fine->world().stores().get<HydrologyStore>().total_surface_m3(),1e-12,"surface projection double counts adaptive cells");
    check(fine->world().coarsen(parent),"coarsening fixture failed");
    near(total_land_water_m3(fine->world(),fine->fields()),total_land_water_m3(restored->world(),restored->fields()),1e-12,"coarsening lost water");
}
void malformed_store_and_flood_feedback() {
    auto sim=fixture();
    auto& store=sim->world().stores().get<HydrologyStore>();
    BinaryWriter writer; store.save(writer); auto bad=writer.data();
    BinaryWriter nan; nan.pod(std::numeric_limits<double>::quiet_NaN());
    std::copy(nan.data().begin(),nan.data().end(),bad.begin()+1);
    bool rejected=false;
    try { BinaryReader reader(bad); store.load(reader,store.snapshot_version()); } catch (const std::exception&) { rejected=true; }
    check(rejected,"hydrology accepted nonfinite budget");
    rejected=false;
    try { store.route(1e30); } catch (const std::invalid_argument&) { rejected=true; }
    check(rejected,"unresolvable routing interval was accepted");
    // Exercise actual ecology turnover, with identical cold forcing and stocks.
    auto dry=make_default_simulation(42,{1,1,3600}),wet=make_default_simulation(42,{1,1,3600});
    auto run_vegetation=[](Simulation& world) {
        Scheduler scheduler;
        GeographyModule().register_systems(scheduler,world.fields());
        MagicModule().register_systems(scheduler,world.fields());
        ClimateModule().register_systems(scheduler,world.fields());
        HydrologyModule().register_systems(scheduler,world.fields());
        EcologyModule().register_systems(scheduler,world.fields()); scheduler.finalize();
        SystemContext ctx{world.world(),world.fields(),10};
        for (auto* system:scheduler.order()) if (system->id()=="ecology.vegetation") system->step(ctx);
    };
    set_all(*wet,"hydrology.flooded_fraction",1); set_all(*wet,"hydrology.inundation_days",30);
    run_vegetation(*dry); run_vegetation(*wet);
    double dry_carbon=0,wet_carbon=0,dry_litter=0,wet_litter=0;
    for (CellId c:dry->world().active_cells()) {
        const auto& d=dry->world().stores().get<FieldStore>(); const auto& w=wet->world().stores().get<FieldStore>();
        dry_carbon+=d.get(c,id(*dry,"ecology.vegetation_carbon_kg")); wet_carbon+=w.get(c,id(*wet,"ecology.vegetation_carbon_kg"));
        dry_litter+=d.get(c,id(*dry,"ecology.litter_carbon_kg")); wet_litter+=w.get(c,id(*wet,"ecology.litter_carbon_kg"));
    }
    check(wet_carbon<dry_carbon && wet_litter>dry_litter,"prolonged flooding did not affect biomass and litter");
}
void physical_geology_and_focus() {
    auto sim=make_default_simulation(42,{1,2,3600});
    const auto original=sim->world().stores().get<HydrologyStore>().nodes();
    sim->set_focus({1,0,0}); sim->step(1);
    sim->clear_focus(); sim->step(1);
    auto& graph=sim->world().stores().get<HydrologyStore>();
    for (std::size_t i=0;i<original.size();++i)
        near(graph.nodes()[i].bed_m,original[i].bed_m,0,"focus changed authoritative hydrological bed");
    auto& fs=sim->world().stores().get<FieldStore>();
    const CellId cell=original[0].cell;
    const double before=fs.get(cell,id(*sim,"geography.elevation_m"));
    fs.add(cell,id(*sim,"geology.crust_thickness_m"),1000);
    sim->step(22);
    const double delta=fs.get(cell,id(*sim,"geography.elevation_m"))-before;
    check(std::abs(delta)>1,"physical geology fixture did not change elevation");
    near(graph.nodes()[0].bed_m,original[0].bed_m+delta,1e-12,"geology did not update hydrological bed by its physical increment");
}
void seasonal_continuation() {
    auto sim=fixture(1);
    const double initial=total_land_water_m3(sim->world(),sim->fields());
    for (int day=0;day<730;++day) {
        const double phase=2*kPi*static_cast<double>(day)/365;
        set_all(*sim,"climate.surface_temperature_k",273+15*std::sin(phase));
        set_all(*sim,"climate.precipitation_mm_day",day%365<180 ? 6.0 : 0.0);
        advance(*sim,1);
        if (day%90==0) {
            assert_budget(*sim,initial);
            const auto snapshot=sim->save_snapshot(); sim->load_snapshot(snapshot);
            check(snapshot==sim->save_snapshot(),"seasonal snapshot changed state");
        }
    }
    assert_budget(*sim,initial);
}
}
int main() {
    try {
        for (const auto& [name,test]:std::vector<std::pair<const char*,void(*)()>>{
            {"snow, recharge and recession",snow_recharge_and_recession},
            {"delayed routing and timestep",delayed_routing_and_timestep},
            {"lakes, spill and terrain change",lakes_spill_connect_and_terrain_change},
            {"mixed cover and continuation",mixed_cover_exchange_and_snapshot},
            {"validation and flood feedback",malformed_store_and_flood_feedback},
            {"physical geology and focus",physical_geology_and_focus},
            {"two seasonal cycles",seasonal_continuation}}) {
            test(); std::cout<<"PASS "<<name<<'\n';
        }
    } catch (const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}

#include "worldsim/c_api.h"
#include "worldsim/climate.hpp"
#include "worldsim/geology.hpp"
#include "worldsim/hydrology.hpp"
#include "worldsim/modules.hpp"
#include "worldsim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <string>

using namespace worldsim;

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void near(double a, double b, double tolerance, const char* message) {
    check(std::isfinite(a) && std::isfinite(b) &&
        std::abs(a-b)<=tolerance*std::max({1.0,std::abs(a),std::abs(b)}),message);
}
template<class F> void rejects(F&& f) {
    bool rejected=false;
    try { f(); } catch (const std::exception&) { rejected=true; }
    check(rejected,"invalid input was accepted");
}
template<class T> void overwrite(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    BinaryWriter writer;
    writer.pod(value);
    std::copy(writer.data().begin(),writer.data().end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset));
}
std::size_t store_offset(const std::vector<std::byte>& bytes, std::string_view wanted) {
    BinaryReader r(bytes);
    for (int i=0;i<8;++i) (void)r.pod<char>();
    (void)r.pod<std::uint32_t>();
    (void)r.pod<std::uint64_t>(); (void)r.pod<std::uint64_t>();
    (void)r.pod<std::uint8_t>(); (void)r.pod<std::uint8_t>();
    (void)r.pod<double>(); (void)r.pod<Tick>();
    if (r.pod<std::uint8_t>()) for (int i=0;i<3;++i) (void)r.pod<double>();
    (void)r.pod<std::uint64_t>();
    const auto commands=r.pod<std::uint64_t>();
    for (std::uint64_t i=0;i<commands;++i) {
        (void)r.pod<Tick>(); (void)r.pod<std::uint64_t>();
        (void)r.pod<std::uint64_t>(); (void)r.pod<FieldId>(); (void)r.pod<double>();
    }
    const auto cells=r.pod<std::uint64_t>();
    for (std::uint64_t i=0;i<cells;++i) (void)r.pod<std::uint64_t>();
    const auto events=r.pod<std::uint64_t>();
    for (std::uint64_t i=0;i<events;++i) {
        (void)r.pod<Tick>(); (void)r.string(); (void)r.pod<std::uint64_t>();
        (void)r.pod<std::uint64_t>(); (void)r.pod<double>();
    }
    const auto stores=r.pod<std::uint32_t>();
    for (std::uint32_t i=0;i<stores;++i) {
        const auto key=r.string();
        (void)r.pod<std::uint32_t>();
        const auto start=bytes.size()-r.remaining()+sizeof(std::uint64_t);
        (void)r.bytes();
        if (key==wanted) return start;
    }
    throw std::runtime_error("test store not found");
}
void run_system(Simulation& sim, std::string_view id, double dt_days) {
    Scheduler scheduler;
    GeographyModule().register_systems(scheduler,sim.fields());
    MagicModule().register_systems(scheduler,sim.fields());
    ClimateModule().register_systems(scheduler,sim.fields());
    HydrologyModule().register_systems(scheduler,sim.fields());
    EcologyModule().register_systems(scheduler,sim.fields());
    scheduler.finalize();
    SystemContext ctx{sim.world(),sim.fields(),dt_days};
    for (auto* system:scheduler.order()) if (system->id()==id) {
        system->step(ctx);
        return;
    }
    throw std::runtime_error("test system not found");
}

void geology_crust_mass_lod() {
    auto sim=make_terrain_simulation(42,{1,2,3600.0});
    auto& world=sim->world();
    auto& fs=world.stores().get<FieldStore>();
    const auto thickness=*sim->fields().find("geology.crust_thickness_m");
    const auto density=*sim->fields().find("geology.crust_density_kg_m3");
    const CellId parent=*world.active_cells().begin();
    world.refine(parent);
    double mass=0.0,volume=0.0;
    unsigned i=0;
    for (CellId child:parent.children()) {
        fs.set(child,thickness,4000.0+10000.0*i);
        fs.set(child,density,3200.0-150.0*i);
        const double v=world.topology().area_m2(child)*fs.get(child,thickness);
        volume+=v;
        mass+=v*fs.get(child,density);
        ++i;
    }
    check(world.coarsen(parent),"coarsen failed");
    near(world.topology().area_m2(parent)*fs.get(parent,thickness),volume,1e-12,
        "LOD changed crust volume");
    near(volume*fs.get(parent,density),mass,1e-12,"LOD changed crust mass");
    world.refine(parent);
    double restored=0.0;
    for (CellId child:parent.children()) restored+=world.topology().area_m2(child)*
        fs.get(child,thickness)*fs.get(child,density);
    near(restored,mass,1e-12,"refinement changed crust mass");
}
void geology_ocean_buoyancy() {
    GeologyModel model(42);
    const Vec3d direction=normalized(Vec3d{1,0.2,0.3});
    GeologyState state{7000,2950,0,40,0,0};
    const double base=model.surface_elevation_m(state,direction,1e10);
    state.crust_thickness_m+=1000;
    check(model.surface_elevation_m(state,direction,1e10)>base+50,
        "ocean surface ignores crust thickening");
    state.crust_thickness_m=7000;
    state.crust_density_kg_m3-=100;
    check(model.surface_elevation_m(state,direction,1e10)>base+50,
        "ocean surface ignores crust density");
}
void geology_erosion_floor() {
    GeologyModel model(42);
    GeologyState state{3000,2800,1,500,0,2};
    const auto budget=model.erode(state,1e10,10);
    near(budget.transported_mass_kg(),0,1e-12,"eroded protected crust");
    near(state.regolith_thickness_m,2,1e-12,"unrealized erosion consumed regolith");
}
void climate_seasons() {
    auto sim=make_default_simulation(42,{1,1,3600.0});
    auto& fs=sim->world().stores().get<FieldStore>();
    const auto solar=*sim->fields().find("climate.solar_flux_w_m2");
    for (Tick day:{Tick{172},Tick{355}}) {
        sim->world().set_tick(day*24);
        run_system(*sim,"climate.surface",1.0/24.0);
        double north=0.0,south=0.0;
        std::size_t north_count=0,south_count=0;
        for (CellId cell:sim->world().active_cells()) {
            const double lat=sim->world().topology().lat_lon_rad(cell).first;
            if (lat>0.3) { north+=fs.get(cell,solar); ++north_count; }
            if (lat<-0.3) { south+=fs.get(cell,solar); ++south_count; }
        }
        check(north_count>0 && south_count>0,"season fixture lacks hemispheres");
        const double signed_difference=
            north/static_cast<double>(north_count)-
            south/static_cast<double>(south_count);
        check(
            signed_difference*(day==172 ? 1.0 : -1.0)>100.0,
            "solar season disagrees with solstice hemisphere"
        );
    }
}
void snapshot_failure_is_atomic() {
    auto sim=make_default_simulation(42,{1,2,3600.0});
    sim->set_focus({1,0,0});
    sim->step(6);
    sim->schedule_field_impulse(20,*sim->world().active_cells().begin(),"magic.mana_j",100);
    sim->world().emit({6,"audit.event",*sim->world().active_cells().begin(),3,1});
    const auto original=sim->save_snapshot();
    auto other=make_default_simulation(42,{1,2,3600.0});
    auto invalid=other->save_snapshot();
    const auto fields=store_offset(invalid,"core.fields");
    // The first field value is invalid, after the field store has cleared itself.
    overwrite(invalid,fields+8+8+4,std::numeric_limits<double>::quiet_NaN());
    rejects([&]{sim->load_snapshot(invalid);});
    check(sim->save_snapshot()==original,"failed snapshot load changed live world");
    invalid=other->save_snapshot();
    const auto cohorts=store_offset(invalid,"ecology.cohorts");
    // Valid CellId, but not an active leaf: failure during final store validation.
    overwrite(invalid,cohorts+8+8+8+8,CellId::make(0,2,0,0).raw());
    rejects([&]{sim->load_snapshot(invalid);});
    check(sim->save_snapshot()==original,"invalid cohort cover changed live world");
    other->load_snapshot(original);
    sim->step(24); other->step(24);
    check(sim->save_snapshot()==other->save_snapshot(),"rejected load changed continuation");
}
void snapshot_focus_validation() {
    auto sim=make_terrain_simulation(42,{0,0,3600});
    sim->set_focus({1,0,0});
    const auto original=sim->save_snapshot();
    for (double value:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid=original;
        overwrite(invalid,47,value); // First focus coordinate after fixed header and flag.
        rejects([&]{sim->load_snapshot(invalid);});
        check(sim->save_snapshot()==original,"invalid focus changed snapshot");
    }
    sim->set_focus({std::numeric_limits<double>::denorm_min(),0,0});
    const auto small_focus=sim->save_snapshot();
    sim->load_snapshot(small_focus);
    check(sim->save_snapshot()==small_focus,"subnormal focus did not round-trip");
}
void numeric_input_validation() {
    rejects([]{Simulation sim(42,{0,0,std::numeric_limits<double>::infinity()});});
    auto sim=make_terrain_simulation(42,{0,0,3600});
    const auto original=sim->save_snapshot();
    rejects([&]{sim->schedule_field_impulse(0,*sim->world().active_cells().begin(),
        "geology.sediment_mass_kg",std::numeric_limits<double>::quiet_NaN());});
    check(sim->save_snapshot()==original,"invalid command was queued");
    sim->step();
    sim->world().set_tick(std::numeric_limits<Tick>::max());
    const auto final_tick=sim->save_snapshot();
    rejects([&]{sim->step();});
    check(sim->save_snapshot()==final_tick,"tick overflow changed state");
}

void snapshot_metadata_validation() {
    auto sim=make_terrain_simulation(42,{0,0,3600});
    sim->set_focus({1,0,0});
    const CellId cell=*sim->world().active_cells().begin();
    sim->schedule_field_impulse(100,cell,"geology.sediment_mass_kg",1);
    const auto original=sim->save_snapshot();
    auto unchanged=[&](const auto& invalid) {
        rejects([&]{sim->load_snapshot(invalid);});
        check(sim->save_snapshot()==original,"invalid metadata changed live state");
    };
    // Epoch-16 framing: fixed header/flag (47), focus (24), next sequence (8),
    // command count (8), then a 36-byte impulse record.
    auto invalid=original;
    overwrite(invalid,115,std::numeric_limits<double>::quiet_NaN());
    unchanged(invalid);
    invalid=original;
    overwrite(invalid,95,std::uint64_t{1}); // Must be less than next sequence.
    unchanged(invalid);
    invalid=original;
    overwrite(invalid,79,std::numeric_limits<std::uint64_t>::max());
    unchanged(invalid);
    invalid=original;
    overwrite(invalid,store_offset(invalid,"core.fields")-12,std::uint32_t{999});
    unchanged(invalid);
    for (std::size_t length:{std::size_t{0},std::size_t{8},std::size_t{47},original.size()-1}) {
        invalid=original;
        invalid.resize(length);
        unchanged(invalid);
    }
    invalid=original;
    invalid.push_back(std::byte{0});
    unchanged(invalid);
}
void field_default_validation() {
    for (double value:{std::numeric_limits<double>::quiet_NaN(),2.0}) {
        FieldRegistry registry;
        rejects([&]{registry.register_field({"test","1",FieldSemantics::Intensive,value,0,1});});
        check(registry.size()==0,"invalid descriptor changed registry");
    }
    FieldRegistry registry;
    rejects([&]{registry.register_field({"nan.bounds","1",FieldSemantics::Intensive,0,
        std::numeric_limits<double>::quiet_NaN(),1});});
    const auto unbounded=registry.register_field({"unbounded","1",FieldSemantics::Intensive,1});
    rejects([&]{registry.register_field({"weighted","1",FieldSemantics::Intensive,1,0,10,unbounded});});
    FieldRegistry other;
    other.register_field({"unbounded","1",FieldSemantics::Intensive,2});
    check(other.schema_hash()!=registry.schema_hash(),"schema ignores default behavior");
}
void topology_direction_validation() {
    CubeSphereTopology topology;
    for (Vec3d direction:{Vec3d{0,0,0},Vec3d{std::numeric_limits<double>::quiet_NaN(),0,0},
                         Vec3d{std::numeric_limits<double>::infinity(),0,0}})
        rejects([&]{(void)topology.from_direction(direction,4);});
    check(topology.from_direction({1e300,1e300,0},4)==topology.from_direction({1,1,0},4),
        "large finite direction changed cell");
    check(topology.from_direction({1e-300,1e-300,0},4)==topology.from_direction({1,1,0},4),
        "small finite direction changed cell");
}

void vegetation_carbon_budget() {
    for (double dt:{1.0,10000.0}) {
        auto sim=make_default_simulation(42,{0,0,3600});
        auto& fs=sim->world().stores().get<FieldStore>();
        const CellId cell=*sim->world().active_cells().begin();
        for (const char* key:{"ecology.grass_carbon_kg","ecology.shrub_carbon_kg","ecology.tree_carbon_kg",
                             "ecology.vegetation_carbon_kg","ecology.litter_carbon_kg"})
            for (CellId c:sim->world().active_cells()) fs.set(c,*sim->fields().find(key),0);
        const auto grass=*sim->fields().find("ecology.grass_carbon_kg");
        const auto carbon=*sim->fields().find("ecology.vegetation_carbon_kg");
        const auto litter=*sim->fields().find("ecology.litter_carbon_kg");
        fs.set(cell,*sim->fields().find("geography.land_fraction"),1);
        fs.set(cell,*sim->fields().find("climate.surface_temperature_k"),360);
        fs.set(cell,grass,1000);
        fs.set(cell,carbon,1000);
        run_system(*sim,"ecology.vegetation",dt);
        const double npp=fs.get(cell,*sim->fields().find("ecology.npp_kg_day"));
        near(1000+npp*dt,fs.get(cell,carbon)+fs.get(cell,litter),1e-10,
            "vegetation NPP/turnover spends more carbon than available");
    }
}
void magic_forcing_uses_elapsed_time() {
    constexpr std::uint64_t seed=424242;
    const auto forcing_at_six_hours=[&](double tick_seconds, Tick tick) {
        auto sim=make_default_simulation(seed,{0,0,tick_seconds});
        auto& fs=sim->world().stores().get<FieldStore>();
        const CellId cell=*sim->world().active_cells().begin();
        const FieldId mana=*sim->fields().find("magic.mana_j");
        const double area=sim->world().topology().area_m2(cell);
        const double baseline=area*2.0e6;
        fs.set(cell,mana,baseline);
        sim->world().set_tick(tick);

        const double dt_days=
            6.0*tick_seconds/86'400.0;
        run_system(*sim,"magic.flux",dt_days);

        const double alpha=
            1.0-std::exp(-0.02*dt_days);
        const double equilibrium=
            baseline+(fs.get(cell,mana)-baseline)/alpha;
        return equilibrium/(area*2.0e6);
    };

    const double hourly=forcing_at_six_hours(3'600.0,5);
    const double half_hourly=forcing_at_six_hours(1'800.0,11);
    near(
        hourly,
        half_hourly,
        1.0e-10,
        "magic forcing phase depends on base tick count instead of elapsed time"
    );
}
void hydrology_water_budget() {
    auto sim=make_default_simulation(42,{0,0,3600});
    auto& fs=sim->world().stores().get<FieldStore>();
    for (CellId cell:sim->world().active_cells()) {
        fs.set(cell,*sim->fields().find("climate.surface_temperature_k"),250);
        fs.set(cell,*sim->fields().find("climate.precipitation_mm_day"),100);
    }
    for (double dt:{0.25,2.0}) {
        const double before=total_land_water_m3(sim->world(),sim->fields());
        const auto budget_before=sim->world().stores().get<HydrologyStore>().budget();
        run_system(*sim,"hydrology.balance",dt);
        const auto budget=sim->world().stores().get<HydrologyStore>().budget();
        near(before+budget.precipitation_m3-budget_before.precipitation_m3,
            total_land_water_m3(sim->world(),sim->fields())+
            budget.evaporation_m3-budget_before.evaporation_m3+
            budget.ocean_export_m3-budget_before.ocean_export_m3,1e-12,"hydrology water balance failed");
    }
}

void c_api_rejects_invalid_inputs() {
    std::unique_ptr<ws_handle,decltype(&ws_destroy)> handle(ws_create_default(42),ws_destroy);
    check(handle!=nullptr,"C ABI creation failed");
    check(ws_load_snapshot_file(handle.get(),nullptr)==0,"C ABI accepted null snapshot path");
    ws_cell_v1 cell{};
    check(ws_copy_cells(handle.get(),&cell,1)==1,"C ABI cell query failed");
    check(ws_schedule_field_impulse(handle.get(),0,cell.cell_id,0,std::numeric_limits<double>::quiet_NaN())==0,
        "C ABI queued non-finite command");
    check(ws_step(handle.get(),1)==1,"C ABI invalid command poisoned future steps");
}

void multiseed_year_continuation() {
    for (std::uint64_t seed:{0U,42U,999U}) {
        auto sim=make_default_simulation(seed,{1,3,3600});
        for (int season=0;season<4;++season) {
            if (season%2==0) sim->set_focus({1,0.3,0.2});
            else sim->clear_focus();
            sim->step(24*92);
            auto& fs=sim->world().stores().get<FieldStore>();
            for (FieldId f=0;f<sim->fields().size();++f) {
                const auto& d=sim->fields().descriptor(f);
                for (double v:fs.column(f)) check(std::isfinite(v) && v>=d.min_value && v<=d.max_value,
                    "annual integration violated field bounds");
            }
            for (const auto& [id,c]:sim->world().stores().get<CohortStore>().all()) {
                (void)id;
                check(std::isfinite(c.count) && c.count>=0,"annual integration produced invalid fauna");
                check(sim->world().active_cells().contains(c.cell),"annual integration left inactive cohort");
            }
            const auto snapshot=sim->save_snapshot();
            auto resumed=make_default_simulation(seed,{1,3,3600});
            resumed->load_snapshot(snapshot);
            sim->step(24); resumed->step(24);
            check(sim->save_snapshot()==resumed->save_snapshot(),"annual integration lost continuation state");
        }
    }
}
}
int main() {
    const std::pair<const char*,std::function<void()>> tests[]{
        {"geology crust mass across LOD",geology_crust_mass_lod},
        {"geology ocean buoyancy",geology_ocean_buoyancy},
        {"geology erosion floor",geology_erosion_floor},
        {"climate seasons",climate_seasons},
        {"snapshot atomic failure",snapshot_failure_is_atomic},
        {"snapshot focus validation",snapshot_focus_validation},
        {"snapshot metadata validation",snapshot_metadata_validation},
        {"numeric input validation",numeric_input_validation},
        {"field default validation",field_default_validation},
        {"topology direction validation",topology_direction_validation},
        {"magic forcing elapsed time",magic_forcing_uses_elapsed_time},
        {"vegetation carbon budget",vegetation_carbon_budget},
        {"hydrology water budget",hydrology_water_budget},
        {"C ABI invalid inputs",c_api_rejects_invalid_inputs},
        {"multiseed year continuation",multiseed_year_continuation}
    };
    int failures=0;
    for (const auto& [name,test]:tests) {
        try { test(); std::cout<<"PASS "<<name<<'\n'; }
        catch (const std::exception& e) { ++failures; std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n'; }
    }
    return failures ? 1 : 0;
}

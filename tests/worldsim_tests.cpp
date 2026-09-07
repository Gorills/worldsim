#include "worldsim/c_api.h"
#include "worldsim/modules.hpp"
#include "worldsim/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace worldsim;

namespace {
void check(bool condition,const char* msg) { if (!condition) throw std::runtime_error(msg); }
void near(double a,double b,double rel,const char* msg) {
    const double scale=std::max({1.0,std::abs(a),std::abs(b)});
    if (std::abs(a-b)>rel*scale) throw std::runtime_error(msg);
}

class TestExtensionSystem final : public ISimSystem {
public:
    explicit TestExtensionSystem(FieldId field): field_(field) {}
    [[nodiscard]] std::string_view id() const override { return "test.extension.system"; }
    [[nodiscard]] SystemAccess access() const override { return {{},{"test.extension.energy"}}; }
    void step(SystemContext& ctx) override {
        auto& fs=ctx.world.stores().get<FieldStore>();
        for (CellId c:ctx.world.active_cells()) fs.add(c,field_,10.0);
    }
private:
    FieldId field_{};
};


class CadenceProbeSystem final : public ISimSystem {
public:
    [[nodiscard]] std::string_view id() const override { return "test.cadence.probe"; }
    [[nodiscard]] Tick cadence_ticks() const override { return 24; }
    [[nodiscard]] SystemAccess access() const override { return {}; }
    void step(SystemContext& ctx) override { ++runs; last_dt_days=ctx.dt_days; }
    int runs{};
    double last_dt_days{};
};

class TestExtensionModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "test.extension"; }
    void register_fields(FieldRegistry& r) override {
        field_=r.register_field({"test.extension.energy","J",FieldSemantics::Extensive,0.0,0.0,1.0e30});
    }
    void register_systems(Scheduler& s,const FieldRegistry&) override {
        s.add(std::make_unique<TestExtensionSystem>(field_));
    }
private:
    FieldId field_{};
};


void test_scheduler_cadence_alignment() {
    Scheduler scheduler;
    auto probe=std::make_unique<CadenceProbeSystem>();
    auto* raw=probe.get();
    scheduler.add(std::move(probe));
    scheduler.finalize();

    FieldRegistry fields;
    fields.freeze();
    WorldState world(1);
    SystemContext ctx{world,fields,1.0/24.0};

    for (Tick tick=0;tick<23;++tick) scheduler.run(tick,ctx);
    check(raw->runs==0,"cadence-24 system ran before 24 base ticks elapsed");
    scheduler.run(23,ctx);
    check(raw->runs==1,"cadence-24 system did not run at end of first window");
    near(raw->last_dt_days,1.0,1e-15,"cadence-24 system received wrong accumulated dt");
    for (Tick tick=24;tick<47;++tick) scheduler.run(tick,ctx);
    check(raw->runs==1,"cadence-24 system ran early in second window");
    scheduler.run(47,ctx);
    check(raw->runs==2,"cadence-24 system did not run at end of second window");
}

void test_cube_sphere() {
    CubeSphereTopology topo;
    const auto cells=uniform_cover(4);
    double area=0.0;
    for (CellId c:cells) {
        check(c.valid(),"invalid uniform cell");
        area+=topo.area_m2(c);
        for (CellId n:topo.neighbors4(c)) check(n.valid() && n.level()==c.level(),"invalid neighbor");
    }
    near(area,4.0*kPi*kEarthRadiusM*kEarthRadiusM,1e-12,"cube-sphere area does not close");
}

void test_lod_conservation() {
    FieldRegistry r;
    const auto ext=r.register_field({"test.mass","kg",FieldSemantics::Extensive,0.0,0.0,1e20});
    const auto intens=r.register_field({"test.temp","K",FieldSemantics::Intensive,0.0,-1000,1000});
    r.freeze();
    WorldState w(1);
    auto& fs=w.stores().emplace<FieldStore>(r);
    auto& cs=w.stores().emplace<CohortStore>();
    w.initialize_cover(0);
    const CellId p=CellId::make(0,0,0,0);
    fs.set(p,ext,12345.0); fs.set(p,intens,289.25);
    cs.add({0,0,p,9,1,777.0,10.0,1.0});
    w.refine(p);
    double mass=0.0;
    for (CellId c:p.children()) { mass+=fs.get(c,ext); near(fs.get(c,intens),289.25,1e-14,"intensive refine changed"); }
    near(mass,12345.0,1e-12,"extensive field not conserved on refine");
    near(cs.total_count(),777.0,1e-12,"cohort count not conserved on refine");
    check(w.coarsen(p),"coarsen failed");
    near(fs.get(p,ext),12345.0,1e-12,"extensive field not conserved on coarsen");
    near(fs.get(p,intens),289.25,1e-12,"intensive field not conserved on coarsen");
    near(cs.total_count(),777.0,1e-12,"cohort count not conserved on coarsen");
}

void test_determinism_and_snapshot() {
    auto a=make_default_simulation(123);
    auto b=make_default_simulation(123);
    a->set_focus({1,0.2,0.1}); b->set_focus({1,0.2,0.1});
    a->step(240); b->step(240);
    check(a->save_snapshot()==b->save_snapshot(),"same seed/input did not reproduce within this build");
    const auto mana=*a->fields().find("magic.mana_j");
    const CellId future_target=a->world().topology().from_direction({1.0,0.2,0.1},a->config().base_level);
    a->schedule_field_impulse(a->world().tick()+2,future_target,mana,9.0e11);
    a->world().emit({a->world().tick(),"test.pending_event",future_target,77,3.5});
    auto snap=a->save_snapshot();
    const Tick tick=a->world().tick();
    a->step(72);
    a->load_snapshot(snap);
    check(a->world().tick()==tick,"snapshot tick roundtrip failed");
    check(a->save_snapshot()==snap,"snapshot roundtrip changed state");

    auto resumed=make_default_simulation(123);
    resumed->load_snapshot(snap);
    a->step(12); resumed->step(12);
    check(a->save_snapshot()==resumed->save_snapshot(),"snapshot continuation lost runtime state or queued commands");
}

void test_ecology_invariants() {
    auto sim=make_default_simulation(999);
    sim->set_focus({0.1,1.0,0.3});
    sim->step(24*60);
    const auto& fs=sim->world().stores().get<FieldStore>();
    const auto water=*sim->fields().find("hydrology.soil_water_m3");
    const auto veg=*sim->fields().find("ecology.vegetation_carbon_kg");
    const auto mana=*sim->fields().find("magic.mana_j");
    for (CellId c:sim->world().active_cells()) {
        check(std::isfinite(fs.get(c,water)) && fs.get(c,water)>=0.0,"invalid water");
        check(std::isfinite(fs.get(c,veg)) && fs.get(c,veg)>=0.0,"invalid vegetation");
        check(std::isfinite(fs.get(c,mana)) && fs.get(c,mana)>=0.0,"invalid mana");
    }
    for (const auto& [id,c]:sim->world().stores().get<CohortStore>().all()) {
        (void)id; check(std::isfinite(c.count) && c.count>=0.0,"invalid cohort count");
    }
}


void test_lod_stability_and_snapshot_across_cover_change() {
    auto sim=make_default_simulation(321);
    sim->set_focus({1.0,0.1,0.05});
    sim->step(1);
    const auto stable_cover=sim->world().active_cells();
    check(stable_cover.size()>uniform_cover(4).size(),"focus did not refine world cover");
    sim->step(1);
    check(sim->world().active_cells()==stable_cover,"unchanged focus caused LOD oscillation");

    const auto snap=sim->save_snapshot();
    sim->set_focus({-1.0,0.2,0.1});
    sim->step(2);
    check(sim->world().active_cells()!=stable_cover,"test did not create a different LOD cover");
    sim->load_snapshot(snap);
    check(sim->world().active_cells()==stable_cover,"snapshot did not restore original adaptive cover");
    check(sim->save_snapshot()==snap,"snapshot restore from different LOD changed canonical state");
}

void test_command_routing_across_lod() {
    auto control=make_default_simulation(654);
    auto commanded=make_default_simulation(654);
    control->set_focus({1.0,0.0,0.0});
    commanded->set_focus({1.0,0.0,0.0});
    control->step(1);
    commanded->step(1);

    const auto mana=*commanded->fields().find("magic.mana_j");
    const CellId coarse_target=commanded->world().topology().from_direction({1.0,0.0,0.0},4);
    check(!commanded->world().active_cells().contains(coarse_target),"command-routing test target was not refined");
    constexpr double delta=1.0e12;
    commanded->schedule_field_impulse(commanded->world().tick(),coarse_target,mana,delta);
    control->step(1);
    commanded->step(1);

    check(commanded->world().active_cells()==control->world().active_cells(),"command changed spatial cover");
    const auto& commanded_fields=commanded->world().stores().get<FieldStore>();
    const auto& control_fields=control->world().stores().get<FieldStore>();
    double observed=0.0;
    for (CellId c:commanded->world().active_cells()) observed+=commanded_fields.get(c,mana)-control_fields.get(c,mana);
    near(observed,delta,1e-12,"LOD-routed field impulse lost or duplicated quantity");
}

void test_columnar_field_store_and_cohort_index() {
    FieldRegistry r;
    const auto f=r.register_field({"test.value","1",FieldSemantics::Intensive,2.0,-100.0,100.0});
    r.freeze();
    WorldState w(77);
    auto& fs=w.stores().emplace<FieldStore>(r);
    auto& cs=w.stores().emplace<CohortStore>();
    w.initialize_cover(0);
    const CellId p=CellId::make(0,0,0,0);
    cs.add({0,0,p,42,1,25.0,2.0,0.5});
    w.refine(p);
    check(fs.dense_cells().size()==w.active_cells().size(),"columnar field store cell count diverged from active cover");
    check(fs.column(f).size()==fs.dense_cells().size(),"column width diverged from dense cell count");
    std::size_t indexed=0;
    for (CellId c:p.children()) indexed+=cs.in_cell(c).size();
    check(indexed==4,"cohort spatial index lost refined cohorts");
    check(w.coarsen(p),"cohort-index test coarsen failed");
    check(cs.in_cell(p).size()==1,"cohort spatial index did not merge to parent");
}

void test_c_api() {
    ws_handle* h=ws_create_default(7);
    check(h!=nullptr,"C API create failed");
    check(ws_abi_version()==1,"C ABI version mismatch");
    check(ws_set_focus(h,1,0,0)==1,"C API focus failed");
    check(ws_step(h,48)==1,"C API step failed");
    const size_t count=ws_cell_count(h);
    check(count>0,"C API no cells");
    std::vector<ws_cell_v1> cells(count);
    check(ws_copy_cells(h,cells.data(),cells.size())==count,"C API cell copy failed");
    uint32_t field=0;
    check(ws_find_field(h,"climate.surface_temperature_k",&field)==1,"C API field lookup failed");
    check(ws_field_count(h)>=10,"C API field introspection count failed");
    const size_t key_size=ws_field_key(h,field,nullptr,0);
    check(key_size>1,"C API field key sizing failed");
    std::vector<char> key(key_size);
    check(ws_field_key(h,field,key.data(),key.size())==key_size,"C API field key copy failed");
    check(std::string(key.data())=="climate.surface_temperature_k","C API field key mismatch");
    ws_field_semantics_v1 semantics=WS_FIELD_EXTENSIVE_V1;
    check(ws_field_semantics(h,field,&semantics)==1 && semantics==WS_FIELD_INTENSIVE_V1,"C API field semantics failed");
    std::vector<double> values(count);
    check(ws_copy_field_values(h,field,values.data(),values.size())==count,"C API field copy failed");

    check(ws_clear_events(h)==1,"C API event clear failed");
    check(ws_event_count(h)==0,"C API event clear did not empty queue");
    ws_destroy(h);
}

void test_module_extension_contract() {
    SimulationConfig cfg;
    cfg.base_level=0;
    cfg.max_level=1;
    cfg.tick_seconds=3600.0;
    Simulation sim(12345,cfg);
    sim.add_module(std::make_unique<TestExtensionModule>());
    sim.build();

    const auto field=sim.fields().find("test.extension.energy");
    check(field.has_value(),"extension module field was not registered");
    sim.step(1);
    const auto& fs=sim.world().stores().get<FieldStore>();
    double total=0.0;
    for (CellId c:sim.world().active_cells()) total+=fs.get(c,*field);
    near(total,60.0,1e-14,"extension module system did not execute through generic core");

    const auto snap=sim.save_snapshot();
    Simulation restored(12345,cfg);
    restored.add_module(std::make_unique<TestExtensionModule>());
    restored.build();
    restored.load_snapshot(snap);
    check(restored.save_snapshot()==snap,"extension module state did not round-trip through generic snapshot machinery");
}
}

int main() {
    try {
        test_scheduler_cadence_alignment();
        test_cube_sphere();
        test_lod_conservation();
        test_determinism_and_snapshot();
        test_lod_stability_and_snapshot_across_cover_change();
        test_command_routing_across_lod();
        test_columnar_field_store_and_cohort_index();
        test_ecology_invariants();
        test_c_api();
        test_module_extension_contract();
        std::cout << "worldsim_tests: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "worldsim_tests: FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

#include "worldsim/c_api.h"
#include "worldsim/modules.hpp"
#include "worldsim/simulation.hpp"
#include "worldsim/terrain.hpp"
#include "worldsim/tectonics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

void test_focus_validation() {
    Simulation sim(5);
    sim.build();

    bool rejected_nan=false;
    try {
        sim.set_focus({std::numeric_limits<double>::quiet_NaN(),0.0,0.0});
    } catch (const std::invalid_argument&) {
        rejected_nan=true;
    }
    check(rejected_nan,"non-finite focus vector was accepted");

    bool rejected_infinity=false;
    try {
        sim.set_focus({std::numeric_limits<double>::infinity(),0.0,0.0});
    } catch (const std::invalid_argument&) {
        rejected_infinity=true;
    }
    check(rejected_infinity,"infinite focus vector was accepted");
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

void test_lod_hysteresis() {
    SimulationConfig cfg;
    cfg.base_level=4;
    cfg.max_level=5;
    cfg.tick_seconds=3600.0;
    Simulation sim(11,cfg);
    sim.build();

    const CellId parent=CellId::make(0,4,8,8);
    const Vec3d center=sim.world().topology().center_unit(parent);
    Vec3d tangent=cross(center,Vec3d{0.0,0.0,1.0});
    if (norm(tangent)<1e-9) tangent=cross(center,Vec3d{0.0,1.0,0.0});
    tangent=normalized(tangent);
    auto focus_at=[&](double degrees) {
        const double radians=degrees*kPi/180.0;
        return normalized(center*std::cos(radians)+tangent*std::sin(radians));
    };

    sim.set_focus(focus_at(8.0));
    sim.step(1);
    check(!sim.world().active_cells().contains(parent),"LOD hysteresis test did not refine target parent");

    sim.set_focus(focus_at(13.0));
    sim.step(1);
    check(!sim.world().active_cells().contains(parent),"LOD coarsened inside hysteresis band");

    sim.set_focus(focus_at(22.0));
    sim.step(1);
    check(sim.world().active_cells().contains(parent),"LOD did not coarsen after leaving hysteresis band");
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

void test_tectonic_model_partition_and_determinism() {
    const TectonicModel a(42);
    const TectonicModel b(42);
    const TectonicModel other_seed(43);

    for (std::uint32_t i=0;i<TectonicModel::kPlateCount;++i) {
        const auto& plate=a.plates()[i];
        const auto at_seed=a.sample_direction(plate.seed_direction);
        check(at_seed.plate_id==i,"plate seed is not owned by its plate");
    }

    CubeSphereTopology topology;
    bool differs_across_seed=false;
    bool observed_positive_forcing=false;
    bool observed_negative_forcing=false;
    bool observed_uplift=false;
    bool observed_divergence=false;
    double nearest_boundary=kPi;
    double min_macro=std::numeric_limits<double>::infinity();
    double max_macro=-std::numeric_limits<double>::infinity();
    double crust_area_m2=0.0;
    double total_area_m2=0.0;
    std::array<double,TectonicModel::kPlateCount> min_affinity{};
    std::array<double,TectonicModel::kPlateCount> max_affinity{};
    min_affinity.fill(1.0);
    max_affinity.fill(0.0);

    for (CellId cell:uniform_cover(5)) {
        const Vec3d p=topology.center_unit(cell);
        const TectonicSample sample=a.sample_direction(p);
        const TectonicSample repeat=b.sample_direction(p);
        const TectonicSample changed=other_seed.sample_direction(p);

        check(sample.plate_id<TectonicModel::kPlateCount,"tectonic plate id out of range");
        check(sample.neighbor_plate_id<TectonicModel::kPlateCount,
              "tectonic neighbor plate id out of range");
        check(sample.neighbor_plate_id!=sample.plate_id,
              "tectonic nearest boundary references the owning plate");
        check(std::isfinite(sample.boundary_distance_rad) &&
              sample.boundary_distance_rad>=0.0 &&
              sample.boundary_distance_rad<=0.5*kPi,
              "invalid tectonic boundary distance");
        check(std::isfinite(sample.convergence) &&
              sample.convergence>=-1.0 && sample.convergence<=1.0,
              "invalid tectonic convergence");
        check(std::isfinite(sample.shear) &&
              sample.shear>=-1.0 && sample.shear<=1.0,
              "invalid tectonic shear");
        check(std::isfinite(sample.boundary_forcing) &&
              sample.boundary_forcing>=-1.0 && sample.boundary_forcing<=1.0,
              "invalid tectonic boundary forcing");
        check(std::abs(sample.boundary_forcing)<=std::abs(sample.convergence)+1.0e-15,
              "tectonic forcing exceeds raw convergence");
        if (std::abs(sample.boundary_forcing)>1.0e-12)
            check(sample.boundary_forcing*sample.convergence>0.0,
                  "tectonic forcing changed convergence sign");

        check(std::isfinite(sample.continental_affinity) &&
              sample.continental_affinity>=0.0 && sample.continental_affinity<=1.0,
              "invalid continental affinity");
        check(std::isfinite(sample.uplift_forcing) &&
              sample.uplift_forcing>=0.0 && sample.uplift_forcing<=1.0,
              "invalid uplift forcing");
        check(std::isfinite(sample.divergence_forcing) &&
              sample.divergence_forcing>=0.0 && sample.divergence_forcing<=1.0,
              "invalid divergence forcing");
        check(std::isfinite(sample.macro_elevation_m) &&
              sample.macro_elevation_m>=-6'000.0 && sample.macro_elevation_m<=6'500.0,
              "invalid tectonic macro elevation");

        check(sample.plate_id==repeat.plate_id &&
              sample.neighbor_plate_id==repeat.neighbor_plate_id,
              "tectonic ownership is not deterministic");
        near(sample.boundary_distance_rad,repeat.boundary_distance_rad,1e-15,
             "tectonic boundary distance is not deterministic");
        near(sample.convergence,repeat.convergence,1e-15,
             "tectonic convergence is not deterministic");
        near(sample.shear,repeat.shear,1e-15,
             "tectonic shear is not deterministic");
        near(sample.boundary_forcing,repeat.boundary_forcing,1e-15,
             "tectonic forcing is not deterministic");
        near(sample.continental_affinity,repeat.continental_affinity,1e-15,
             "continental affinity is not deterministic");
        near(sample.uplift_forcing,repeat.uplift_forcing,1e-15,
             "tectonic uplift is not deterministic");
        near(sample.divergence_forcing,repeat.divergence_forcing,1e-15,
             "tectonic divergence is not deterministic");
        near(sample.macro_elevation_m,repeat.macro_elevation_m,1e-12,
             "tectonic macro elevation is not deterministic");

        const double area=topology.area_m2(cell);
        crust_area_m2+=area*sample.continental_affinity;
        total_area_m2+=area;
        min_affinity[sample.plate_id]=std::min(
            min_affinity[sample.plate_id],
            sample.continental_affinity
        );
        max_affinity[sample.plate_id]=std::max(
            max_affinity[sample.plate_id],
            sample.continental_affinity
        );
        min_macro=std::min(min_macro,sample.macro_elevation_m);
        max_macro=std::max(max_macro,sample.macro_elevation_m);
        nearest_boundary=std::min(nearest_boundary,sample.boundary_distance_rad);
        observed_positive_forcing|=sample.boundary_forcing>1.0e-4;
        observed_negative_forcing|=sample.boundary_forcing<-1.0e-4;
        observed_uplift|=sample.uplift_forcing>0.05;
        observed_divergence|=sample.divergence_forcing>0.05;
        differs_across_seed|=sample.plate_id!=changed.plate_id ||
            std::abs(sample.boundary_forcing-changed.boundary_forcing)>1.0e-6 ||
            std::abs(sample.continental_affinity-changed.continental_affinity)>1.0e-6;
    }

    check(nearest_boundary<0.02,"tectonic cover test did not resolve any plate boundary");
    check(observed_positive_forcing,"tectonic model produced no convergent forcing");
    check(observed_negative_forcing,"tectonic model produced no divergent forcing");
    check(observed_uplift,"tectonic macro model produced no uplift zones");
    check(observed_divergence,"tectonic macro model produced no divergence zones");
    check(differs_across_seed,"tectonic model does not vary with world seed");

    const double crust_fraction=crust_area_m2/total_area_m2;
    check(crust_fraction>0.15 && crust_fraction<0.50,
          "continuous continental crust fraction is outside the intended broad range");
    bool plate_contains_mixed_crust=false;
    for (std::uint32_t i=0;i<TectonicModel::kPlateCount;++i)
        plate_contains_mixed_crust|=max_affinity[i]-min_affinity[i]>0.75;
    check(plate_contains_mixed_crust,
          "continental affinity collapsed back into a per-plate crust type");
    check(min_macro<-3'000.0 && max_macro>1'000.0,
          "tectonic macro preview does not contain both deep ocean and high relief");

    // Regression for the old hard 0.45 competition gate. This direction lies
    // on one of its pair-admission contours for seed 42; the old implementation
    // changed uplift by ~0.64 and macro height by ~1.1 km across this tiny step.
    const Vec3d gate_point{
        -0.8915729528965797,
        0.2272902103838312,
        0.39171013508326175
    };
    const Vec3d gate_tangent{
        -0.4504176476975162,
        -0.5350529101595087,
        -0.7147323456878645
    };
    constexpr double gate_epsilon=1.0e-7;
    const TectonicSample gate_left=a.sample_direction(normalized(
        gate_point*std::cos(gate_epsilon)+gate_tangent*std::sin(gate_epsilon)
    ));
    const TectonicSample gate_right=a.sample_direction(normalized(
        gate_point*std::cos(gate_epsilon)-gate_tangent*std::sin(gate_epsilon)
    ));
    check(std::abs(gate_left.uplift_forcing-gate_right.uplift_forcing)<1.0e-3,
          "tectonic uplift has a hard pair-competition seam");
    check(std::abs(gate_left.divergence_forcing-gate_right.divergence_forcing)<1.0e-3,
          "tectonic divergence has a hard pair-competition seam");
    check(std::abs(gate_left.macro_elevation_m-gate_right.macro_elevation_m)<5.0,
          "tectonic macro relief has a hard pair-competition seam");

    // The crust field is intentionally broad-scale, but it must not regress to
    // the visually obvious union of a handful of radial spherical caps. Count
    // 0.5-isoline crossings in the same coarse equirectangular grid used by the
    // Godot smoke path. Seed 42 had 74 crossings with the old three-lobe
    // provinces; the sphere-native coherent field is deliberately more irregular.
    constexpr int crust_map_width=64;
    constexpr int crust_map_height=32;
    int crust_edge_crossings=0;
    for (int y=0;y<crust_map_height;++y) {
        const double v=(static_cast<double>(y)+0.5)/static_cast<double>(crust_map_height);
        const double latitude=(0.5-v)*kPi;
        const double sin_lat=std::sin(latitude);
        const double cos_lat=std::cos(latitude);
        for (int x=0;x<crust_map_width;++x) {
            const auto sample_at=[&](int sample_x) {
                const double u=(static_cast<double>(sample_x)+0.5)/
                    static_cast<double>(crust_map_width);
                const double longitude=(2.0*u-1.0)*kPi;
                return a.sample_direction({
                    cos_lat*std::cos(longitude),
                    cos_lat*std::sin(longitude),
                    sin_lat
                }).continental_affinity;
            };
            const bool current=sample_at(x)>=0.5;
            const bool next=sample_at((x+1)%crust_map_width)>=0.5;
            if (current!=next) ++crust_edge_crossings;
        }
    }
    check(crust_edge_crossings>=100,
          "continental affinity regressed to overly simple spherical lobes");

    // Crust affinity must stay continuous when plate ownership changes. Project
    // one resolved near-boundary sample onto its exact owner/neighbor bisector,
    // then sample a tiny angular step on both sides.
    bool continuity_checked=false;
    for (CellId cell:uniform_cover(5)) {
        const Vec3d p=topology.center_unit(cell);
        const TectonicSample sample=a.sample_direction(p);
        if (sample.boundary_distance_rad>0.01) continue;

        const Vec3d plane_normal=normalized(
            a.plates()[sample.plate_id].seed_direction-
            a.plates()[sample.neighbor_plate_id].seed_direction
        );
        const Vec3d boundary_point=normalized(p-plane_normal*dot(p,plane_normal));
        Vec3d tangent=plane_normal-boundary_point*dot(boundary_point,plane_normal);
        tangent=normalized(tangent);
        constexpr double epsilon=1.0e-6;
        const TectonicSample left=a.sample_direction(normalized(
            boundary_point*std::cos(epsilon)+tangent*std::sin(epsilon)
        ));
        const TectonicSample right=a.sample_direction(normalized(
            boundary_point*std::cos(epsilon)-tangent*std::sin(epsilon)
        ));
        if (left.plate_id==right.plate_id) continue;
        check(std::abs(left.continental_affinity-right.continental_affinity)<1.0e-3,
              "continental affinity is discontinuous at a plate boundary");
        check(std::abs(left.macro_elevation_m-right.macro_elevation_m)<5.0,
              "tectonic macro relief is discontinuous at a plate boundary");
        continuity_checked=true;
        break;
    }
    check(continuity_checked,"tectonic crust continuity test found no resolved boundary");
}

void test_tectonic_model_multiseed_robustness() {
    constexpr std::uint64_t seed_count=64;
    constexpr int sample_count=512;
    constexpr double golden_angle_rad=2.3999632297286533222;
    constexpr double sample_phase_rad=0.731;

    for (std::uint64_t seed=0;seed<seed_count;++seed) {
        const TectonicModel tectonics(seed);

        double affinity_sum=0.0;
        int high_affinity_count=0;
        int transition_count=0;
        int uplift_count=0;
        int divergence_count=0;
        int positive_macro_count=0;
        int deep_ocean_count=0;

        Vec3d nearest_direction{};
        TectonicSample nearest_sample{};
        double nearest_distance=kPi;

        for (int i=0;i<sample_count;++i) {
            const double z=1.0-2.0*(static_cast<double>(i)+0.5)/
                static_cast<double>(sample_count);
            // Deliberately rotate the validation lattice away from the
            // constructor's 128-point calibration lattice so the test does not
            // validate the normalization on its own quadrature orientation.
            const double phi=sample_phase_rad+
                static_cast<double>(i)*golden_angle_rad;
            const double radial=std::sqrt(std::max(0.0,1.0-z*z));
            const Vec3d direction{
                z,
                radial*std::cos(phi),
                radial*std::sin(phi)
            };
            const TectonicSample sample=tectonics.sample_direction(direction);

            check(std::isfinite(sample.continental_affinity),
                  "multi-seed tectonics produced non-finite crust affinity");
            check(std::isfinite(sample.uplift_forcing) &&
                  std::isfinite(sample.divergence_forcing),
                  "multi-seed tectonics produced non-finite boundary response");
            check(std::isfinite(sample.macro_elevation_m),
                  "multi-seed tectonics produced non-finite macro elevation");

            affinity_sum+=sample.continental_affinity;
            high_affinity_count+=sample.continental_affinity>=0.5 ? 1 : 0;
            transition_count+=(
                sample.continental_affinity>0.05 &&
                sample.continental_affinity<0.95
            ) ? 1 : 0;
            uplift_count+=sample.uplift_forcing>0.05 ? 1 : 0;
            divergence_count+=sample.divergence_forcing>0.05 ? 1 : 0;
            positive_macro_count+=sample.macro_elevation_m>0.0 ? 1 : 0;
            deep_ocean_count+=sample.macro_elevation_m<-3'000.0 ? 1 : 0;

            if (sample.boundary_distance_rad<nearest_distance) {
                nearest_distance=sample.boundary_distance_rad;
                nearest_direction=direction;
                nearest_sample=sample;
            }
        }

        const double inverse_count=1.0/static_cast<double>(sample_count);
        const double mean_affinity=affinity_sum*inverse_count;
        const double high_affinity_fraction=
            static_cast<double>(high_affinity_count)*inverse_count;
        const double transition_fraction=
            static_cast<double>(transition_count)*inverse_count;
        const double uplift_fraction=static_cast<double>(uplift_count)*inverse_count;
        const double divergence_fraction=
            static_cast<double>(divergence_count)*inverse_count;
        const double positive_macro_fraction=
            static_cast<double>(positive_macro_count)*inverse_count;
        const double deep_ocean_fraction=
            static_cast<double>(deep_ocean_count)*inverse_count;

        check(mean_affinity>0.15 && mean_affinity<0.45,
              "multi-seed crust mean collapsed toward all-ocean or all-continent");
        check(high_affinity_fraction>0.12 && high_affinity_fraction<0.48,
              "multi-seed crust high-affinity area is degenerate");
        check(transition_fraction>0.10,
              "multi-seed crust lost a meaningful transitional belt");
        check(uplift_fraction>0.07 && divergence_fraction>0.07,
              "multi-seed tectonics lost uplift or divergence coverage");
        check(positive_macro_fraction>0.10,
              "multi-seed macro relief lost positive terrain");
        check(deep_ocean_fraction>0.50 && deep_ocean_fraction<0.90,
              "multi-seed macro relief collapsed toward one elevation regime");

        check(nearest_distance<0.01,
              "multi-seed continuity probe did not resolve a plate boundary");
        const Vec3d plane_normal=normalized(
            tectonics.plates()[nearest_sample.plate_id].seed_direction-
            tectonics.plates()[nearest_sample.neighbor_plate_id].seed_direction
        );
        const Vec3d boundary_point=normalized(
            nearest_direction-plane_normal*dot(nearest_direction,plane_normal)
        );
        Vec3d tangent=plane_normal-boundary_point*dot(boundary_point,plane_normal);
        tangent=normalized(tangent);

        constexpr double epsilon=1.0e-6;
        const TectonicSample left=tectonics.sample_direction(normalized(
            boundary_point*std::cos(epsilon)+tangent*std::sin(epsilon)
        ));
        const TectonicSample right=tectonics.sample_direction(normalized(
            boundary_point*std::cos(epsilon)-tangent*std::sin(epsilon)
        ));
        check(left.plate_id!=right.plate_id,
              "multi-seed continuity probe did not straddle plate ownership");
        check(std::abs(left.continental_affinity-right.continental_affinity)<1.0e-3,
              "multi-seed crust affinity is discontinuous at a plate boundary");
        check(std::abs(left.macro_elevation_m-right.macro_elevation_m)<5.0,
              "multi-seed macro relief is discontinuous at a plate boundary");
    }
}

void test_procedural_terrain_scale_and_determinism() {
    const TerrainGenerator terrain(42);
    const TerrainSample center=terrain.sample_projected(0.0,0.0);
    const TerrainSample repeat=terrain.sample_projected(0.0,0.0);
    const TerrainSample remote_ocean=terrain.sample_projected(8'000'000.0,4'000'000.0);

    near(center.elevation_m,repeat.elevation_m,1e-15,"terrain generator is not deterministic");
    near(center.land_fraction,repeat.land_fraction,1e-15,"terrain land mask is not deterministic");
    check(center.land_fraction>0.95 && center.elevation_m>0.0,"continent center is not land");
    check(remote_ocean.land_fraction<0.05 && remote_ocean.elevation_m<0.0,"remote terrain is not ocean floor");

    CubeSphereTopology topology;
    double land_area_m2=0.0;
    for (CellId cell:uniform_cover(5)) {
        const TerrainSample sample=terrain.sample_direction(topology.center_unit(cell));
        land_area_m2+=topology.area_m2(cell)*sample.land_fraction;
    }
    constexpr double kMinEurasiaScaleM2=40.0e12;
    constexpr double kMaxEurasiaScaleM2=70.0e12;
    check(land_area_m2>=kMinEurasiaScaleM2 && land_area_m2<=kMaxEurasiaScaleM2,
          "procedural continent is not Eurasia-scale");
}

void test_sphere_native_terrain_continuity() {
    const TerrainGenerator terrain(42);
    const Vec3d center=TerrainGenerator::projected_to_direction(0.0,0.0);
    const Vec3d antipode=center*(-1.0);
    Vec3d tangent_a=cross(antipode,Vec3d{0.0,0.0,1.0});
    if (norm(tangent_a)<1.0e-9) tangent_a=cross(antipode,Vec3d{0.0,1.0,0.0});
    tangent_a=normalized(tangent_a);
    const Vec3d tangent_b=normalized(cross(antipode,tangent_a));
    constexpr double epsilon_rad=1.0e-7;

    const TerrainSample at_antipode=terrain.sample_direction(antipode);
    double min_elevation=at_antipode.elevation_m;
    double max_elevation=at_antipode.elevation_m;
    for (Vec3d tangent:{tangent_a,tangent_a*(-1.0),tangent_b,tangent_b*(-1.0)}) {
        const Vec3d direction=normalized(
            antipode*std::cos(epsilon_rad)+tangent*std::sin(epsilon_rad)
        );
        const TerrainSample sample=terrain.sample_direction(direction);
        check(sample.land_fraction<0.01,"terrain antipode unexpectedly became land");
        min_elevation=std::min(min_elevation,sample.elevation_m);
        max_elevation=std::max(max_elevation,sample.elevation_m);
    }
    check(max_elevation-min_elevation<1.0,
          "terrain is discontinuous around the local-projection antipode");

    constexpr double east_m=12'345.0;
    constexpr double north_m=-6'789.0;
    const TerrainSample projected=terrain.sample_projected(east_m,north_m);
    const TerrainSample directional=terrain.sample_direction(
        TerrainGenerator::projected_to_direction(east_m,north_m)
    );
    near(projected.elevation_m,directional.elevation_m,1e-15,
         "projected terrain sampling diverged from authoritative sphere sampling");
    near(projected.land_fraction,directional.land_fraction,1e-15,
         "projected terrain land mask diverged from authoritative sphere sampling");
}

void test_geography_refinement_samples_new_detail() {
    SimulationConfig cfg;
    cfg.base_level=4;
    cfg.max_level=5;
    cfg.tick_seconds=3600.0;
    auto sim=make_terrain_simulation(42,cfg);
    const TerrainGenerator terrain(42);
    const Vec3d focus=TerrainGenerator::projected_to_direction(0.0,0.0);
    const CellId parent=sim->world().topology().from_direction(focus,cfg.base_level);
    const auto children=parent.children();
    const auto elevation=*sim->fields().find("geography.elevation_m");

    sim->set_focus(focus);
    sim->step(1);

    const auto& fields=sim->world().stores().get<FieldStore>();
    bool differs_from_parent_copy=false;
    const double old_parent_value=terrain.sample_direction(sim->world().topology().center_unit(parent)).elevation_m;
    for (CellId child:children) {
        check(sim->world().active_cells().contains(child),"focused geography parent was not refined");
        const double expected=terrain.sample_direction(sim->world().topology().center_unit(child)).elevation_m;
        near(fields.get(child,elevation),expected,1e-14,
             "refined geography did not resample authoritative terrain");
        if (std::abs(expected-old_parent_value)>1e-6) differs_from_parent_copy=true;
    }
    check(differs_from_parent_copy,"terrain regression test did not observe subcell detail");
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
        test_focus_validation();
        test_cube_sphere();
        test_lod_conservation();
        test_lod_hysteresis();
        test_determinism_and_snapshot();
        test_lod_stability_and_snapshot_across_cover_change();
        test_command_routing_across_lod();
        test_columnar_field_store_and_cohort_index();
        test_ecology_invariants();
        test_tectonic_model_partition_and_determinism();
        test_tectonic_model_multiseed_robustness();
        test_procedural_terrain_scale_and_determinism();
        test_sphere_native_terrain_continuity();
        test_geography_refinement_samples_new_detail();
        test_c_api();
        test_module_extension_contract();
        std::cout << "worldsim_tests: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "worldsim_tests: FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

#include "worldsim/c_api.h"
#include "worldsim/geology.hpp"
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
#include <map>
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

class ZeroRunoffModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override {
        return "test.zero_runoff";
    }
    void register_fields(FieldRegistry& r) override {
        r.register_field({
            "hydrology.runoff_m3_day",
            "m3/day",
            FieldSemantics::Extensive,
            0.0,
            0.0,
            1.0e30
        });
    }
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

void test_adaptive_cover_resolution() {
    WorldState fine_neighbor_world(7);
    fine_neighbor_world.initialize_cover(1);

    const CellId source=CellId::make(0,1,0,0);
    const CellId right=
        fine_neighbor_world.topology().neighbors4(source)[1];
    check(
        right==CellId::make(0,1,1,0),
        "adaptive-cover fixture did not select in-face neighbor"
    );

    fine_neighbor_world.refine(right);
    const auto parts=fine_neighbor_world.resolve_active_cover(right);
    check(
        parts.size()==4,
        "refined region did not resolve to all active children"
    );
    double weight_sum=0.0;
    double area_sum=0.0;
    for (const ActiveCoverPart& part:parts) {
        check(
            fine_neighbor_world.active_cells().contains(part.cell),
            "adaptive-cover resolution returned inactive leaf"
        );
        check(
            part.cell.parent()==right,
            "adaptive-cover resolution escaped requested region"
        );
        check(
            part.weight>0.0,
            "adaptive-cover resolution returned non-positive weight"
        );
        weight_sum+=part.weight;
        area_sum+=fine_neighbor_world.topology().area_m2(part.cell);
    }
    near(
        weight_sum,
        1.0,
        1.0e-15,
        "adaptive-cover weights did not close"
    );
    for (const ActiveCoverPart& part:parts) {
        near(
            part.weight,
            fine_neighbor_world.topology().area_m2(part.cell)/area_sum,
            1.0e-14,
            "adaptive-cover fine-leaf weight is not area proportional"
        );
    }

    const auto fine_sides=fine_neighbor_world.active_neighbors4(source);
    check(
        fine_sides[1].size()==4,
        "active neighbor query collapsed refined interface"
    );
    for (std::size_t i=0;i<parts.size();++i) {
        check(
            fine_sides[1][i].cell==parts[i].cell,
            "active neighbor query changed deterministic leaf order"
        );
        near(
            fine_sides[1][i].weight,
            parts[i].weight,
            1.0e-15,
            "active neighbor query changed region weights"
        );
    }

    WorldState coarse_neighbor_world(8);
    coarse_neighbor_world.initialize_cover(1);
    coarse_neighbor_world.refine(source);
    const CellId fine_source=source.children()[1];
    check(
        coarse_neighbor_world.active_cells().contains(fine_source),
        "coarse-neighbor fixture did not refine source"
    );
    const auto coarse_sides=
        coarse_neighbor_world.active_neighbors4(fine_source);
    check(
        coarse_sides[1].size()==1 &&
        coarse_sides[1][0].cell==right,
        "fine source did not resolve neighboring region to coarse ancestor"
    );
    near(
        coarse_sides[1][0].weight,
        1.0,
        1.0e-15,
        "coarse ancestor did not represent full neighboring region"
    );
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

    SimulationConfig coarse_cfg;
    coarse_cfg.base_level=0;
    coarse_cfg.max_level=1;
    coarse_cfg.tick_seconds=3600.0;
    Simulation coarse(12,coarse_cfg);
    coarse.build();
    const CellId coarse_parent=CellId::make(0,0,0,0);
    const Vec3d coarse_focus=
        coarse.world().topology().center_unit(coarse_parent);
    coarse.set_focus(coarse_focus);
    coarse.step(1);
    check(
        !coarse.world().active_cells().contains(coarse_parent),
        "coarse LOD parent refined and coarsened in the same update"
    );
    const auto coarse_stable_cover=coarse.world().active_cells();
    coarse.step(1);
    check(
        coarse.world().active_cells()==coarse_stable_cover,
        "unchanged coarse focus caused LOD oscillation"
    );
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

    // Authoritative domain semantics change across snapshot epochs. Older
    // snapshots may omit or encode state under previous domain contracts, so
    // reject them rather than mixing incompatible world state.
    check(snap.size()>11,"snapshot header is unexpectedly short");
    for (std::uint8_t legacy_version:{
        std::uint8_t{2},std::uint8_t{3},std::uint8_t{4},std::uint8_t{5},
        std::uint8_t{6},std::uint8_t{7},std::uint8_t{8},std::uint8_t{9},
        std::uint8_t{10},std::uint8_t{11},std::uint8_t{12},
        std::uint8_t{13},std::uint8_t{14},std::uint8_t{15},
        std::uint8_t{16},std::uint8_t{17},std::uint8_t{18},
        std::uint8_t{19},std::uint8_t{20},std::uint8_t{21},
        std::uint8_t{22},std::uint8_t{23},std::uint8_t{24},
        std::uint8_t{25},std::uint8_t{26},std::uint8_t{27},
        std::uint8_t{28},std::uint8_t{29},std::uint8_t{30},
        std::uint8_t{31}
    }) {
        auto legacy_snapshot=snap;
        legacy_snapshot[8]=static_cast<std::byte>(legacy_version);
        bool rejected_legacy_snapshot=false;
        try {
            auto legacy_target=make_default_simulation(123);
            legacy_target->load_snapshot(legacy_snapshot);
        } catch (const std::runtime_error&) {
            rejected_legacy_snapshot=true;
        }
        check(rejected_legacy_snapshot,
              "stale authoritative-terrain snapshot epoch was accepted");
    }

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
    const auto fertility=*sim->fields().find("ecology.soil_fertility");
    const auto litter=*sim->fields().find("ecology.litter_carbon_kg");
    const auto grass=*sim->fields().find("ecology.grass_carbon_kg");
    const auto shrub=*sim->fields().find("ecology.shrub_carbon_kg");
    const auto tree=*sim->fields().find("ecology.tree_carbon_kg");
    const auto veg=*sim->fields().find("ecology.vegetation_carbon_kg");
    const auto mana=*sim->fields().find("magic.mana_j");
    for (CellId c:sim->world().active_cells()) {
        check(std::isfinite(fs.get(c,water)) && fs.get(c,water)>=0.0,"invalid water");
        check(
            std::isfinite(fs.get(c,fertility)) &&
            fs.get(c,fertility)>=0.0 &&
            fs.get(c,fertility)<=1.0,
            "invalid soil fertility"
        );
        check(
            std::isfinite(fs.get(c,litter)) &&
            fs.get(c,litter)>=0.0,
            "invalid litter carbon"
        );
        const double grass_c=fs.get(c,grass);
        const double shrub_c=fs.get(c,shrub);
        const double tree_c=fs.get(c,tree);
        check(
            std::isfinite(grass_c) && grass_c>=0.0 &&
            std::isfinite(shrub_c) && shrub_c>=0.0 &&
            std::isfinite(tree_c) && tree_c>=0.0,
            "invalid plant functional type carbon"
        );
        check(
            std::isfinite(fs.get(c,veg)) && fs.get(c,veg)>=0.0,
            "invalid vegetation"
        );
        near(
            fs.get(c,veg),
            grass_c+shrub_c+tree_c,
            1.0e-12,
            "total vegetation diverged from PFT carbon pools"
        );
        check(std::isfinite(fs.get(c,mana)) && fs.get(c,mana)>=0.0,"invalid mana");
    }
    for (const auto& [id,c]:sim->world().stores().get<CohortStore>().all()) {
        (void)id; check(std::isfinite(c.count) && c.count>=0.0,"invalid cohort count");
    }
}

void test_flora_pft_contracts() {
    SimulationConfig cfg;
    cfg.base_level=2;
    cfg.max_level=2;
    cfg.tick_seconds=3600.0;
    constexpr std::uint64_t seed=4242;

    auto disable_fauna=[](Simulation& sim) {
        auto& cohorts=sim.world().stores().get<CohortStore>();
        for (CellId cell:sim.world().active_cells()) {
            for (auto& ref:cohorts.in_cell(cell))
                ref.get().count=0.0;
        }
    };

    auto find_land_pair=[](Simulation& sim) {
        const auto land=*sim.fields().find("geography.land_fraction");
        const auto fertility=*sim.fields().find("ecology.soil_fertility");
        const auto& fs=sim.world().stores().get<FieldStore>();
        for (CellId source:sim.world().active_cells()) {
            if (
                fs.get(source,land)<0.75 ||
                fs.get(source,fertility)<0.15
            ) {
                continue;
            }
            for (const auto& side:sim.world().active_neighbors4(source)) {
                if (side.size()!=1) continue;
                const CellId target=side.front().cell;
                if (
                    fs.get(target,land)>=0.75 &&
                    fs.get(target,fertility)>=0.15
                ) {
                    return std::pair{source,target};
                }
            }
        }
        throw std::runtime_error(
            "flora fixture found no suitable neighboring land cells"
        );
    };

    auto sterile=make_default_simulation(seed,cfg);
    auto colonizing=make_default_simulation(seed,cfg);
    disable_fauna(*sterile);
    disable_fauna(*colonizing);

    const auto grass=*sterile->fields().find("ecology.grass_carbon_kg");
    const auto shrub=*sterile->fields().find("ecology.shrub_carbon_kg");
    const auto tree=*sterile->fields().find("ecology.tree_carbon_kg");
    const auto total=*sterile->fields().find("ecology.vegetation_carbon_kg");
    const auto land=*sterile->fields().find("geography.land_fraction");

    auto& sterile_fs=sterile->world().stores().get<FieldStore>();
    auto& colonizing_fs=colonizing->world().stores().get<FieldStore>();
    for (CellId cell:sterile->world().active_cells()) {
        for (FieldId field:{grass,shrub,tree,total}) {
            sterile_fs.set(cell,field,0.0);
            colonizing_fs.set(cell,field,0.0);
        }
    }

    const auto [source,target]=find_land_pair(*colonizing);
    const double source_effective_area=
        colonizing->world().topology().area_m2(source)*
        colonizing_fs.get(source,land);
    colonizing_fs.set(
        source,
        grass,
        0.50*source_effective_area
    );
    colonizing_fs.set(
        source,
        total,
        0.50*source_effective_area
    );

    sterile->step(24);
    colonizing->step(24);

    for (CellId cell:sterile->world().active_cells()) {
        near(
            sterile_fs.get(cell,grass)+
            sterile_fs.get(cell,shrub)+
            sterile_fs.get(cell,tree),
            0.0,
            1.0e-15,
            "sterile plant cover generated biomass without propagules"
        );
    }
    check(
        colonizing_fs.get(target,grass)>0.0,
        "neighboring grass source did not establish in empty habitat"
    );

    auto open=make_default_simulation(seed,cfg);
    auto wooded=make_default_simulation(seed,cfg);
    disable_fauna(*open);
    disable_fauna(*wooded);
    const auto [competition_cell,unused_neighbor]=find_land_pair(*open);
    (void)unused_neighbor;

    auto& open_fs=open->world().stores().get<FieldStore>();
    auto& wooded_fs=wooded->world().stores().get<FieldStore>();
    const double effective_area=
        open->world().topology().area_m2(competition_cell)*
        open_fs.get(competition_cell,land);
    const double grass_start=0.60*effective_area;

    for (auto* fields:{&open_fs,&wooded_fs}) {
        fields->set(competition_cell,grass,grass_start);
        fields->set(competition_cell,shrub,0.0);
    }
    open_fs.set(competition_cell,tree,0.0);
    open_fs.set(competition_cell,total,grass_start);
    wooded_fs.set(competition_cell,tree,4.0*effective_area);
    wooded_fs.set(
        competition_cell,
        total,
        grass_start+4.0*effective_area
    );

    open->step(24);
    wooded->step(24);

    check(
        wooded_fs.get(competition_cell,grass)<
        open_fs.get(competition_cell,grass)-
            1.0e-5*effective_area,
        "tree canopy did not suppress grass through succession competition"
    );
    near(
        wooded_fs.get(competition_cell,total),
        wooded_fs.get(competition_cell,grass)+
        wooded_fs.get(competition_cell,shrub)+
        wooded_fs.get(competition_cell,tree),
        1.0e-12,
        "fauna/vegetation pipeline broke PFT total after competition"
    );
}

void test_living_soil_ecology_contracts() {
    constexpr std::uint64_t seed=4242;

    auto probe_cell=[](Simulation& sim) {
        const auto land=*sim.fields().find("geography.land_fraction");
        const auto& fs=sim.world().stores().get<FieldStore>();
        for (CellId cell:sim.world().active_cells()) {
            if (fs.get(cell,land)>0.90)
                return cell;
        }
        throw std::runtime_error(
            "living-soil fixture found no mostly-land cell"
        );
    };

    // Regolith must control root-zone storage rather than every land cell
    // receiving the old fixed 0.35 m water bucket.
    auto thin_soil=make_default_simulation(seed);
    auto deep_soil=make_default_simulation(seed);
    const CellId hydrology_cell=probe_cell(*thin_soil);
    check(
        deep_soil->world().active_cells().contains(hydrology_cell),
        "paired living-soil simulations diverged in initial cover"
    );

    const auto land=*thin_soil->fields().find("geography.land_fraction");
    const auto regolith=*thin_soil->fields().find(
        "geology.regolith_thickness_m"
    );
    const auto water=*thin_soil->fields().find("hydrology.soil_water_m3");
    const auto drainage=*thin_soil->fields().find("hydrology.drainage_since_soil_m3");
    const double effective_area=
        thin_soil->world().topology().area_m2(hydrology_cell)*
        thin_soil->world().stores().get<FieldStore>().get(
            hydrology_cell,
            land
        );

    auto& thin_fs=thin_soil->world().stores().get<FieldStore>();
    auto& deep_fs=deep_soil->world().stores().get<FieldStore>();
    thin_fs.set(hydrology_cell,regolith,0.0);
    deep_fs.set(hydrology_cell,regolith,3.0);
    thin_fs.set(hydrology_cell,water,0.20*effective_area);
    deep_fs.set(hydrology_cell,water,0.20*effective_area);

    thin_soil->step(6);
    deep_soil->step(6);
    check(
        thin_fs.get(hydrology_cell,drainage)>
        deep_fs.get(hydrology_cell,drainage)+
            0.05*effective_area,
        "thin and deep regolith retained nearly the same storm water"
    );

    // The derived fertility diagnostic must reflect a real finite nutrient
    // donor, and that donor must affect plant production through the scheduler.
    auto poor=make_default_simulation(seed);
    auto fertile=make_default_simulation(seed);
    const CellId vegetation_cell=probe_cell(*poor);
    auto& poor_fs=poor->world().stores().get<FieldStore>();
    auto& fertile_fs=fertile->world().stores().get<FieldStore>();
    const auto fertility=*poor->fields().find("ecology.soil_fertility");
    const auto mineral_n=*poor->fields().find("ecology.mineral_nitrogen_kg");
    const auto fast_n=*poor->fields().find("ecology.soil_fast_nitrogen_kg");
    const auto slow_n=*poor->fields().find("ecology.soil_slow_nitrogen_kg");
    const auto litter=*poor->fields().find("ecology.litter_carbon_kg");
    const auto litter_n=*poor->fields().find("ecology.litter_nitrogen_kg");
    const auto vegetation=*poor->fields().find(
        "ecology.vegetation_carbon_kg"
    );
    const auto npp=*poor->fields().find("ecology.npp_kg_day");
    const auto veg_land=*poor->fields().find("geography.land_fraction");
    const double veg_effective_area=
        poor->world().topology().area_m2(vegetation_cell)*
        poor_fs.get(vegetation_cell,veg_land);
    const double controlled_carbon=0.5*veg_effective_area;

    for (auto* fields:{&poor_fs,&fertile_fs}) {
        fields->set(vegetation_cell,fast_n,0.0);
        fields->set(vegetation_cell,slow_n,0.0);
        fields->set(vegetation_cell,litter,0.0);
        fields->set(vegetation_cell,litter_n,0.0);
        fields->set(vegetation_cell,vegetation,controlled_carbon);
    }
    poor_fs.set(vegetation_cell,mineral_n,0.0);
    fertile_fs.set(
        vegetation_cell,mineral_n,0.02*veg_effective_area
    );

    poor->step(24);
    fertile->step(24);
    check(
        fertile_fs.get(vegetation_cell,fertility)>
        poor_fs.get(vegetation_cell,fertility),
        "mineral nitrogen did not raise derived soil fertility"
    );
    check(
        fertile_fs.get(vegetation_cell,npp)>
        poor_fs.get(vegetation_cell,npp)+
            1.0e-5*veg_effective_area,
        "finite mineral nitrogen did not limit vegetation NPP"
    );
    check(
        poor_fs.get(vegetation_cell,litter)>0.0 &&
        fertile_fs.get(vegetation_cell,litter)>0.0,
        "vegetation turnover did not return carbon to litter"
    );

    // Detritus must feed back into the persistent soil state.
    auto bare_litter=make_default_simulation(seed);
    auto rich_litter=make_default_simulation(seed);
    const CellId soil_cell=probe_cell(*bare_litter);
    auto& bare_fs=bare_litter->world().stores().get<FieldStore>();
    auto& rich_fs=rich_litter->world().stores().get<FieldStore>();
    const auto mineralization=*bare_litter->fields().find(
        "ecology.nitrogen_mineralization_kg_day"
    );
    const auto soil_litter=*bare_litter->fields().find(
        "ecology.litter_carbon_kg"
    );
    const auto soil_land=*bare_litter->fields().find(
        "geography.land_fraction"
    );
    const double soil_effective_area=
        bare_litter->world().topology().area_m2(soil_cell)*
        bare_fs.get(soil_cell,soil_land);

    const auto soil_litter_n=*bare_litter->fields().find(
        "ecology.litter_nitrogen_kg"
    );
    const auto soil_fast_n=*bare_litter->fields().find(
        "ecology.soil_fast_nitrogen_kg"
    );
    const auto soil_slow_n=*bare_litter->fields().find(
        "ecology.soil_slow_nitrogen_kg"
    );
    const auto soil_mineral_n=*bare_litter->fields().find(
        "ecology.mineral_nitrogen_kg"
    );
    for (auto* fields:{&bare_fs,&rich_fs}) {
        fields->set(soil_cell,soil_fast_n,0.0);
        fields->set(soil_cell,soil_slow_n,0.0);
        fields->set(soil_cell,soil_mineral_n,0.0);
        fields->set(soil_cell,soil_litter,0.0);
        fields->set(soil_cell,soil_litter_n,0.0);
    }
    const double rich_litter_carbon=2.0*soil_effective_area;
    rich_fs.set(soil_cell,soil_litter,rich_litter_carbon);
    rich_fs.set(
        soil_cell,
        soil_litter_n,
        rich_litter_carbon/40.0
    );

    bare_litter->step(24);
    rich_litter->step(24);
    check(
        rich_fs.get(soil_cell,mineralization)>
        bare_fs.get(soil_cell,mineralization),
        "litter nitrogen did not increase soil mineralization"
    );
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
    SimulationConfig cfg;
    cfg.base_level=4;
    cfg.max_level=5;
    cfg.tick_seconds=3'600.0;

    auto magic_only=[&]() {
        auto simulation=std::make_unique<Simulation>(654,cfg);
        simulation->add_module(std::make_unique<MagicModule>());
        simulation->build();
        return simulation;
    };

    auto command_before_refine=magic_only();
    auto command_after_refine=magic_only();
    auto no_command=magic_only();
    const CellId coarse_target=
        command_before_refine->world().topology().from_direction(
            {1.0,0.0,0.0},
            cfg.base_level
        );
    const Vec3d focus=
        command_before_refine->world().topology().center_unit(
            coarse_target
        );
    const FieldId mana=
        *command_before_refine->fields().find("magic.mana_j");
    const FieldId growth=
        *command_before_refine->fields().find("magic.growth_factor");
    constexpr double mana_delta=1.0e12;
    constexpr double growth_delta=0.20;

    command_before_refine->schedule_field_impulse(
        command_before_refine->world().tick(),
        coarse_target,
        mana,
        mana_delta
    );
    command_before_refine->schedule_field_impulse(
        command_before_refine->world().tick(),
        coarse_target,
        growth,
        growth_delta
    );
    command_before_refine->step(1);
    command_before_refine->set_focus(focus);
    command_before_refine->step(1);

    command_after_refine->set_focus(focus);
    command_after_refine->step(1);
    check(
        !command_after_refine->world().active_cells().contains(
            coarse_target
        ),
        "command-routing fixture did not refine target region"
    );
    command_after_refine->schedule_field_impulse(
        command_after_refine->world().tick(),
        coarse_target,
        mana,
        mana_delta
    );
    command_after_refine->schedule_field_impulse(
        command_after_refine->world().tick(),
        coarse_target,
        growth,
        growth_delta
    );
    command_after_refine->step(1);

    no_command->set_focus(focus);
    no_command->step(2);

    check(
        command_before_refine->world().active_cells()==
            command_after_refine->world().active_cells() &&
        command_after_refine->world().active_cells()==
            no_command->world().active_cells(),
        "equivalent command histories produced different spatial covers"
    );
    const auto parts=
        command_before_refine->world().resolve_active_cover(
            coarse_target
        );
    check(
        parts.size()==4,
        "command-routing fixture did not resolve refined region"
    );

    const auto& before_fields=
        command_before_refine->world().stores().get<FieldStore>();
    const auto& after_fields=
        command_after_refine->world().stores().get<FieldStore>();
    const auto& baseline_fields=
        no_command->world().stores().get<FieldStore>();
    double observed_mana_delta=0.0;
    for (const ActiveCoverPart& part:parts) {
        near(
            before_fields.get(part.cell,mana),
            after_fields.get(part.cell,mana),
            1.0e-14,
            "extensive field impulse depends on refined active LOD"
        );
        near(
            before_fields.get(part.cell,growth),
            after_fields.get(part.cell,growth),
            1.0e-14,
            "intensive field impulse depends on refined active LOD"
        );
        near(
            after_fields.get(part.cell,growth)-
                baseline_fields.get(part.cell,growth),
            growth_delta,
            1.0e-14,
            "intensive field impulse was not applied across target region"
        );
        observed_mana_delta+=
            after_fields.get(part.cell,mana)-
            baseline_fields.get(part.cell,mana);
    }
    near(
        observed_mana_delta,
        mana_delta,
        1.0e-12,
        "LOD-routed extensive field impulse lost or duplicated quantity"
    );

    auto fine_before_coarsen=magic_only();
    auto fine_after_coarsen=magic_only();
    auto fine_baseline=magic_only();
    fine_before_coarsen->set_focus(focus);
    fine_after_coarsen->set_focus(focus);
    fine_baseline->set_focus(focus);
    fine_before_coarsen->step(1);
    fine_after_coarsen->step(1);
    fine_baseline->step(1);

    const CellId fine_target=coarse_target.children()[0];
    check(
        fine_before_coarsen->world().active_cells().contains(fine_target) &&
        fine_after_coarsen->world().active_cells().contains(fine_target),
        "command-routing fixture did not expose fine target cell"
    );

    fine_before_coarsen->schedule_field_impulse(
        fine_before_coarsen->world().tick(),
        fine_target,
        mana,
        mana_delta
    );
    fine_before_coarsen->schedule_field_impulse(
        fine_before_coarsen->world().tick(),
        fine_target,
        growth,
        growth_delta
    );
    fine_before_coarsen->step(1);
    fine_before_coarsen->clear_focus();
    fine_before_coarsen->step(1);

    fine_after_coarsen->schedule_field_impulse(
        fine_after_coarsen->world().tick(),
        fine_target,
        mana,
        mana_delta
    );
    fine_after_coarsen->schedule_field_impulse(
        fine_after_coarsen->world().tick(),
        fine_target,
        growth,
        growth_delta
    );
    fine_after_coarsen->clear_focus();
    fine_after_coarsen->step(2);

    fine_baseline->clear_focus();
    fine_baseline->step(2);

    check(
        fine_before_coarsen->world().active_cells()==
            fine_after_coarsen->world().active_cells() &&
        fine_after_coarsen->world().active_cells()==
            fine_baseline->world().active_cells() &&
        fine_after_coarsen->world().active_cells().contains(coarse_target),
        "fine-command histories did not return to the same coarse cover"
    );
    const auto& fine_before_fields=
        fine_before_coarsen->world().stores().get<FieldStore>();
    const auto& fine_after_fields=
        fine_after_coarsen->world().stores().get<FieldStore>();
    const auto& fine_baseline_fields=
        fine_baseline->world().stores().get<FieldStore>();
    near(
        fine_before_fields.get(coarse_target,mana),
        fine_after_fields.get(coarse_target,mana),
        1.0e-14,
        "extensive fine-region impulse depends on coarsening timing"
    );
    near(
        fine_before_fields.get(coarse_target,growth),
        fine_after_fields.get(coarse_target,growth),
        1.0e-14,
        "intensive fine-region impulse depends on coarsening timing"
    );
    near(
        fine_after_fields.get(coarse_target,mana)-
            fine_baseline_fields.get(coarse_target,mana),
        mana_delta,
        1.0e-12,
        "coarse projection changed extensive fine-region impulse total"
    );
    const double fine_area_fraction=
        fine_after_coarsen->world().topology().area_m2(fine_target)/
        fine_after_coarsen->world().topology().area_m2(coarse_target);
    near(
        fine_after_fields.get(coarse_target,growth)-
            fine_baseline_fields.get(coarse_target,growth),
        growth_delta*fine_area_fraction,
        1.0e-14,
        "coarse projection did not area-restrict intensive fine-region impulse"
    );
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

    check(
        ws_find_field(h,"climate.surface_albedo",&field)==1,
        "C API snow-albedo field lookup failed"
    );
    check(
        ws_field_semantics(h,field,&semantics)==1 &&
            semantics==WS_FIELD_INTENSIVE_V1,
        "C API snow-albedo semantics failed"
    );
    check(
        ws_copy_field_values(h,field,values.data(),values.size())==count,
        "C API snow-albedo copy failed"
    );
    check(
        std::all_of(
            values.begin(),values.end(),
            [](double value) {
                return std::isfinite(value) &&
                    value>=0.0 && value<=1.0;
            }
        ),
        "C API snow-albedo values left registered bounds"
    );

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

    // Regression for the old runner-up ownership-score shortcut. Find a
    // deterministic probe where the second-highest seed score is not the
    // geometrically nearest owner/competitor bisector, then verify that the
    // model reports the bisector rather than the score runner-up. Searching
    // keeps this contract independent of the exact seeded plate layout.
    bool nearest_boundary_case_checked=false;
    constexpr int nearest_boundary_probe_count=4096;
    for (int probe=0;probe<nearest_boundary_probe_count && !nearest_boundary_case_checked;++probe) {
        const double z=1.0-2.0*(static_cast<double>(probe)+0.5)/
            static_cast<double>(nearest_boundary_probe_count);
        const double phi=0.419+static_cast<double>(probe)*2.3999632297286533222;
        const double radial=std::sqrt(std::max(0.0,1.0-z*z));
        const Vec3d direction{
            radial*std::cos(phi),
            radial*std::sin(phi),
            z
        };

        std::uint32_t expected_owner=0;
        for (std::uint32_t i=1;i<TectonicModel::kPlateCount;++i) {
            if (dot(direction,a.plates()[i].seed_direction)>
                dot(direction,a.plates()[expected_owner].seed_direction))
                expected_owner=i;
        }

        std::uint32_t score_runner_up=expected_owner==0 ? 1 : 0;
        std::uint32_t expected_neighbor=score_runner_up;
        double runner_up_score=dot(
            direction,
            a.plates()[score_runner_up].seed_direction
        );
        const auto bisector_distance=[&](std::uint32_t neighbor) {
            const Vec3d normal=normalized(
                a.plates()[expected_owner].seed_direction-
                a.plates()[neighbor].seed_direction
            );
            return std::asin(std::abs(std::clamp(
                dot(direction,normal),
                -1.0,
                1.0
            )));
        };
        double expected_distance=bisector_distance(expected_neighbor);

        for (std::uint32_t i=0;i<TectonicModel::kPlateCount;++i) {
            if (i==expected_owner) continue;
            const double score=dot(direction,a.plates()[i].seed_direction);
            if (i!=score_runner_up && score>runner_up_score) {
                score_runner_up=i;
                runner_up_score=score;
            }
            const double distance=bisector_distance(i);
            if (distance<expected_distance) {
                expected_neighbor=i;
                expected_distance=distance;
            }
        }
        if (score_runner_up==expected_neighbor) continue;

        const TectonicSample sample=a.sample_direction(direction);
        check(sample.plate_id==expected_owner,
              "nearest-boundary regression probe changed owning plate");
        check(sample.neighbor_plate_id==expected_neighbor,
              "tectonic model did not report the geometrically nearest boundary");
        near(sample.boundary_distance_rad,expected_distance,1.0e-15,
             "tectonic nearest-boundary distance is inconsistent with reported neighbor");
        nearest_boundary_case_checked=true;
    }
    check(nearest_boundary_case_checked,
          "nearest-boundary regression could not find a score/bisector disagreement");

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

    // Visual-regression sentinel for overly broad, ribbon-like convergent uplift.
    // Sample the same seed-42 equirectangular field that exposed the artifact.
    // The old uniform 12-degree belt activates about 14.5% of this grid above
    // uplift=0.05 and has a low contour-per-active-cell ratio (~0.74).
    // Orogenic shaping should keep meaningful coverage while producing a thinner,
    // less ribbon-like boundary footprint.
    constexpr int uplift_map_width=128;
    constexpr int uplift_map_height=64;
    std::array<bool,uplift_map_width*uplift_map_height> uplift_active{};
    int uplift_active_count=0;
    for (int y=0;y<uplift_map_height;++y) {
        const double v=(static_cast<double>(y)+0.5)/
            static_cast<double>(uplift_map_height);
        const double latitude=(0.5-v)*kPi;
        const double sin_lat=std::sin(latitude);
        const double cos_lat=std::cos(latitude);
        for (int x=0;x<uplift_map_width;++x) {
            const double u=(static_cast<double>(x)+0.5)/
                static_cast<double>(uplift_map_width);
            const double longitude=(2.0*u-1.0)*kPi;
            const bool active=a.sample_direction({
                cos_lat*std::cos(longitude),
                cos_lat*std::sin(longitude),
                sin_lat
            }).uplift_forcing>=0.05;
            uplift_active[static_cast<std::size_t>(y*uplift_map_width+x)]=active;
            uplift_active_count+=active ? 1 : 0;
        }
    }

    int uplift_contour_edges=0;
    for (int y=0;y<uplift_map_height;++y) {
        for (int x=0;x<uplift_map_width;++x) {
            const auto at=[&](int sx, int sy) {
                const int wrapped_x=(sx+uplift_map_width)%uplift_map_width;
                return uplift_active[static_cast<std::size_t>(
                    sy*uplift_map_width+wrapped_x
                )];
            };
            const bool current=at(x,y);
            if (current!=at(x+1,y)) ++uplift_contour_edges;
            if (y+1<uplift_map_height && current!=at(x,y+1))
                ++uplift_contour_edges;
        }
    }

    const double uplift_active_fraction=
        static_cast<double>(uplift_active_count)/
        static_cast<double>(uplift_map_width*uplift_map_height);
    const double uplift_contour_per_active=
        static_cast<double>(uplift_contour_edges)/
        static_cast<double>(std::max(uplift_active_count,1));
    check(uplift_active_fraction>0.08 && uplift_active_fraction<0.135,
          "convergent uplift regressed to an overly broad boundary ribbon");
    check(uplift_contour_per_active>0.80,
          "convergent uplift footprint is too smooth for orogenic belts");

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

    double plate_area_ratio_sum=0.0;
    double center_spacing_cv_sum=0.0;

    for (std::uint64_t seed=0;seed<seed_count;++seed) {
        const TectonicModel tectonics(seed);

        double affinity_sum=0.0;
        int high_affinity_count=0;
        int transition_count=0;
        int uplift_count=0;
        int divergence_count=0;
        int positive_macro_count=0;
        int deep_ocean_count=0;
        std::array<int,TectonicModel::kPlateCount> plate_sample_counts{};

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
            ++plate_sample_counts[sample.plate_id];

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

        const auto [min_plate_samples,max_plate_samples]=std::minmax_element(
            plate_sample_counts.begin(),plate_sample_counts.end()
        );
        check(*min_plate_samples>0,"multi-seed plate-area probe missed a plate");
        plate_area_ratio_sum+=static_cast<double>(*max_plate_samples)/
            static_cast<double>(*min_plate_samples);

        std::array<double,TectonicModel::kPlateCount> nearest_center_distances{};
        double center_distance_sum=0.0;
        for (std::uint32_t i=0;i<TectonicModel::kPlateCount;++i) {
            double nearest=kPi;
            for (std::uint32_t j=0;j<TectonicModel::kPlateCount;++j) {
                if (i==j) continue;
                nearest=std::min(nearest,std::acos(std::clamp(
                    dot(tectonics.plates()[i].seed_direction,
                        tectonics.plates()[j].seed_direction),
                    -1.0,
                    1.0
                )));
            }
            nearest_center_distances[i]=nearest;
            center_distance_sum+=nearest;
        }
        const double mean_center_distance=center_distance_sum/
            static_cast<double>(TectonicModel::kPlateCount);
        double center_distance_variance=0.0;
        for (double distance:nearest_center_distances) {
            const double delta=distance-mean_center_distance;
            center_distance_variance+=delta*delta;
        }
        center_spacing_cv_sum+=std::sqrt(
            center_distance_variance/static_cast<double>(TectonicModel::kPlateCount)
        )/mean_center_distance;

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

    const double inverse_seed_count=1.0/static_cast<double>(seed_count);
    check(plate_area_ratio_sum*inverse_seed_count>1.80,
          "multi-seed plate areas regressed toward an equal-area partition");
    check(center_spacing_cv_sum*inverse_seed_count>0.10,
          "multi-seed plate centers regressed toward regular spacing");

    // The diversity fix must not trade a regular layout for near-duplicate
    // seeds. Seed 64 is the first deterministic regression on the previous
    // unconstrained +/-0.5 rad jitter (minimum center spacing ~3.3 degrees).
    constexpr std::uint64_t separation_seed_count=256;
    constexpr double minimum_center_separation_rad=10.0*kPi/180.0;
    for (std::uint64_t seed=0;seed<separation_seed_count;++seed) {
        const TectonicModel tectonics(seed);
        for (std::uint32_t i=0;i<TectonicModel::kPlateCount;++i) {
            for (std::uint32_t j=i+1;j<TectonicModel::kPlateCount;++j) {
                const double separation=std::acos(std::clamp(
                    dot(tectonics.plates()[i].seed_direction,
                        tectonics.plates()[j].seed_direction),
                    -1.0,
                    1.0
                ));
                check(separation>minimum_center_separation_rad,
                      "tectonic plate seeds are pathologically clustered");
            }
        }
    }
}

void test_authoritative_terrain_tracks_tectonic_macro_relief() {
    const TerrainGenerator terrain(42);
    const TectonicModel tectonics(42);

    constexpr int sample_count=1024;
    constexpr double golden_angle_rad=2.3999632297286533222;
    constexpr double sample_phase_rad=0.417;

    double terrain_sum=0.0;
    double macro_sum=0.0;
    double terrain_sq_sum=0.0;
    double macro_sq_sum=0.0;
    double product_sum=0.0;
    double residual_abs_sum=0.0;
    double max_residual=0.0;
    bool land_classification_consistent=true;

    for (int i=0;i<sample_count;++i) {
        const double z=1.0-2.0*(static_cast<double>(i)+0.5)/
            static_cast<double>(sample_count);
        const double phi=sample_phase_rad+
            static_cast<double>(i)*golden_angle_rad;
        const double radial=std::sqrt(std::max(0.0,1.0-z*z));
        const Vec3d direction{
            radial*std::cos(phi),
            z,
            radial*std::sin(phi)
        };

        const TerrainSample terrain_sample=terrain.sample_direction(direction);
        const TectonicSample tectonic_sample=tectonics.sample_direction(direction);
        const double authoritative=terrain_sample.elevation_m;
        const double macro=tectonic_sample.macro_elevation_m;
        const double residual=authoritative-macro;

        terrain_sum+=authoritative;
        macro_sum+=macro;
        terrain_sq_sum+=authoritative*authoritative;
        macro_sq_sum+=macro*macro;
        product_sum+=authoritative*macro;
        residual_abs_sum+=std::abs(residual);
        max_residual=std::max(max_residual,std::abs(residual));

        if (authoritative>200.0)
            land_classification_consistent&=terrain_sample.land_fraction>0.99;
        if (authoritative<-200.0)
            land_classification_consistent&=terrain_sample.land_fraction<0.01;
    }

    const double n=static_cast<double>(sample_count);
    const double covariance=product_sum-terrain_sum*macro_sum/n;
    const double terrain_variance=terrain_sq_sum-terrain_sum*terrain_sum/n;
    const double macro_variance=macro_sq_sum-macro_sum*macro_sum/n;
    const double correlation=covariance/std::sqrt(terrain_variance*macro_variance);
    const double mean_abs_residual=residual_abs_sum/n;

    check(correlation>0.95,
          "authoritative terrain low-frequency shape is not driven by tectonic macro relief");
    check(land_classification_consistent,
          "authoritative terrain land fraction disagrees with final elevation");
    check(mean_abs_residual>10.0,
          "authoritative terrain lost meso/local procedural detail");
    check(max_residual<1'500.0,
          "procedural terrain detail overwhelms tectonic macro relief");
}

void test_authoritative_terrain_scale_and_determinism() {
    const TerrainGenerator terrain(42);
    const TerrainSample center=terrain.sample_projected(0.0,0.0);
    const TerrainSample repeat=terrain.sample_projected(0.0,0.0);
    const TerrainSample remote_ocean=terrain.sample_projected(8'000'000.0,4'000'000.0);

    near(center.elevation_m,repeat.elevation_m,1e-15,"terrain generator is not deterministic");
    near(center.land_fraction,repeat.land_fraction,1e-15,"terrain land mask is not deterministic");
    check(center.land_fraction>0.95 && center.elevation_m>200.0,
          "seed-42 local walker origin is not stable dry land");
    check(remote_ocean.land_fraction<0.05 && remote_ocean.elevation_m<-3'000.0,
          "seed-42 remote terrain is not deep ocean");

    CubeSphereTopology topology;
    double land_area_m2=0.0;
    double total_area_m2=0.0;
    for (CellId cell:uniform_cover(5)) {
        const TerrainSample sample=terrain.sample_direction(topology.center_unit(cell));
        const double area=topology.area_m2(cell);
        land_area_m2+=area*sample.land_fraction;
        total_area_m2+=area;
    }
    const double land_fraction=land_area_m2/total_area_m2;
    check(land_fraction>0.15 && land_fraction<0.40,
          "authoritative tectonic terrain has degenerate global land coverage");
}

void test_visual_orography_has_local_mountain_prominence() {
    const TerrainGenerator terrain(42);

    constexpr double mountain_east_m=5'573'000.0;
    constexpr double mountain_north_m=-1'800'300.0;
    constexpr double probe_radius_m=20'000.0;
    constexpr int direction_count=32;

    const TerrainSample authoritative=terrain.sample_projected(
        mountain_east_m,
        mountain_north_m
    );
    const TerrainSample visual=terrain.sample_visual_projected(
        mountain_east_m,
        mountain_north_m
    );
    check(
        visual.elevation_m-authoritative.elevation_m>650.0,
        "visual orography did not raise the reported mountain above its regional terrain"
    );

    double ring_min=std::numeric_limits<double>::infinity();
    double ring_max=-std::numeric_limits<double>::infinity();
    for (int i=0;i<direction_count;++i) {
        const double angle=
            2.0*kPi*static_cast<double>(i)/
            static_cast<double>(direction_count);
        const TerrainSample sample=terrain.sample_visual_projected(
            mountain_east_m+probe_radius_m*std::cos(angle),
            mountain_north_m+probe_radius_m*std::sin(angle)
        );
        ring_min=std::min(ring_min,sample.elevation_m);
        ring_max=std::max(ring_max,sample.elevation_m);
    }
    check(
        visual.elevation_m-ring_min>550.0,
        "reported mountain lacks local peak prominence within 20 km"
    );
    check(
        ring_max-ring_min>850.0,
        "convergent visual terrain remains a broad plateau at mountain scale"
    );

    const TerrainSample origin_authoritative=terrain.sample_projected(0.0,0.0);
    const TerrainSample origin_visual=terrain.sample_visual_projected(0.0,0.0);
    near(
        origin_visual.elevation_m,
        origin_authoritative.elevation_m,
        1e-15,
        "visual orography added generic relief outside an uplift belt"
    );
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

void test_geology_model_process_contracts() {
    const GeologyModel geology(42);
    const TectonicModel& tectonics=geology.tectonics();

    Vec3d most_oceanic{1,0,0};
    Vec3d most_continental{1,0,0};
    Vec3d collision_direction{1,0,0};
    Vec3d rift_direction{1,0,0};
    Vec3d subduction_direction{1,0,0};
    Vec3d volcanic_arc_direction{1,0,0};
    double min_affinity=2.0;
    double max_affinity=-1.0;
    double collision_score=-1.0;
    double rift_score=-1.0;
    double subduction_score=-1.0;
    double volcanic_arc_score=-1.0;
    double trench_peak_distance=kPi;
    double arc_peak_distance=0.0;

    constexpr double area_m2=1.0e10;
    constexpr int sample_count=4096;
    constexpr double golden_angle_rad=2.3999632297286533222;
    for (int i=0;i<sample_count;++i) {
        const double z=1.0-2.0*(static_cast<double>(i)+0.5)/
            static_cast<double>(sample_count);
        const double phi=0.271+static_cast<double>(i)*golden_angle_rad;
        const double radial=std::sqrt(std::max(0.0,1.0-z*z));
        const Vec3d direction{radial*std::cos(phi),radial*std::sin(phi),z};
        const TectonicSample sample=tectonics.sample_direction(direction);
        if (sample.continental_affinity<min_affinity) {
            min_affinity=sample.continental_affinity;
            most_oceanic=direction;
        }
        if (sample.continental_affinity>max_affinity) {
            max_affinity=sample.continental_affinity;
            most_continental=direction;
        }
        const GeologyState state=geology.initial_state(direction,area_m2);
        const BoundaryFeatureSample features=geology.boundary_features(
            state,
            direction
        );
        if (features.collision_forcing>collision_score) {
            collision_score=features.collision_forcing;
            collision_direction=direction;
        }
        if (features.rift_forcing>rift_score) {
            rift_score=features.rift_forcing;
            rift_direction=direction;
        }
        if (features.trench_forcing>subduction_score) {
            subduction_score=features.trench_forcing;
            subduction_direction=direction;
            trench_peak_distance=sample.boundary_distance_rad;
        }
        if (features.volcanic_arc_forcing>volcanic_arc_score) {
            volcanic_arc_score=features.volcanic_arc_forcing;
            volcanic_arc_direction=direction;
            arc_peak_distance=sample.boundary_distance_rad;
        }
    }

    const GeologyState ocean=geology.initial_state(most_oceanic,area_m2);
    const GeologyState continent=geology.initial_state(most_continental,area_m2);
    check(continent.crust_thickness_m>ocean.crust_thickness_m+15'000.0,
          "continental/oceanic crust thickness contrast collapsed");
    check(continent.crust_density_kg_m3<ocean.crust_density_kg_m3,
          "continental crust is not more buoyant than oceanic crust");
    check(continent.continental_fraction>ocean.continental_fraction+0.6,
          "persistent continental fraction lost crust-type contrast");

    GeologyState young_ocean=ocean;
    GeologyState old_ocean=ocean;
    young_ocean.lithosphere_age_ma=1.0;
    old_ocean.lithosphere_age_ma=120.0;
    const double young_elevation=geology.surface_elevation_m(
        young_ocean,most_oceanic,area_m2
    );
    const double old_elevation=geology.surface_elevation_m(
        old_ocean,most_oceanic,area_m2
    );
    check(old_elevation<young_elevation-1'000.0,
          "oceanic thermal subsidence lost age dependence");

    const double shallow_mass=geology.sediment_mass_for_thickness_kg(
        500.0,
        area_m2
    );
    const double deep_mass=geology.sediment_mass_for_thickness_kg(
        3'000.0,
        area_m2
    );
    near(
        geology.sediment_column_thickness_m(shallow_mass,area_m2),
        500.0,
        1.0e-11,
        "sediment compaction mass/thickness inverse failed for shallow column"
    );
    near(
        geology.sediment_column_thickness_m(deep_mass,area_m2),
        3'000.0,
        1.0e-11,
        "sediment compaction mass/thickness inverse failed for deep column"
    );
    const double very_deep_mass=geology.sediment_mass_for_thickness_kg(
        10'000.0,
        area_m2
    );
    near(
        geology.sediment_column_thickness_m(very_deep_mass,area_m2),
        10'000.0,
        1.0e-10,
        "sediment compaction inverse lost convergence for deep burial"
    );
    const double deep_after_same_mass=
        geology.sediment_column_thickness_m(
            deep_mass+shallow_mass,
            area_m2
        );
    check(
        deep_after_same_mass-3'000.0<450.0,
        "deep sediment column did not compact added mass"
    );

    check(
        collision_score>0.02 &&
        rift_score>0.03 &&
        subduction_score>0.02 &&
        volcanic_arc_score>0.02,
        "geology process probes did not resolve required boundary regimes"
    );
    check(arc_peak_distance>trench_peak_distance+0.5*kPi/180.0,
          "volcanic arc is not spatially offset inland from trench forcing");

    GeologyState collision=geology.initial_state(collision_direction,area_m2);
    const double collision_before=collision.crust_thickness_m;
    geology.advance_tectonics(collision,collision_direction,5.0e6);
    check(collision.crust_thickness_m>collision_before,
          "continental convergence did not thicken crust");

    GeologyState rift=geology.initial_state(rift_direction,area_m2);
    const double rift_before=rift.crust_thickness_m;
    const double continental_before=rift.continental_fraction;
    geology.advance_tectonics(rift,rift_direction,5.0e6);
    check(rift.crust_thickness_m<rift_before,
          "continental divergence did not thin crust");
    check(rift.continental_fraction<continental_before,
          "continental divergence did not reduce continental fraction");

    GeologyState breakup=geology.initial_state(rift_direction,area_m2);
    const double breakup_age_before=breakup.lithosphere_age_ma;
    geology.advance_tectonics(breakup,rift_direction,250.0e6);
    check(breakup.continental_fraction<0.45,
          "long-lived rifting did not complete continental breakup");
    check(breakup.crust_thickness_m<15'000.0,
          "completed breakup did not transition toward thin oceanic crust");
    check(breakup.lithosphere_age_ma<breakup_age_before,
          "post-breakup spreading did not renew young lithosphere");

    GeologyState subduction=geology.initial_state(subduction_direction,area_m2);
    const double subduction_before=subduction.crust_thickness_m;
    geology.advance_tectonics(subduction,subduction_direction,5.0e6);
    check(subduction.crust_thickness_m<subduction_before,
          "selected subducting side did not consume crust");

    GeologyState volcanic_arc=geology.initial_state(
        volcanic_arc_direction,
        area_m2
    );
    const double volcanic_arc_before=volcanic_arc.crust_thickness_m;
    geology.advance_tectonics(volcanic_arc,volcanic_arc_direction,5.0e6);
    check(volcanic_arc.crust_thickness_m>volcanic_arc_before,
          "overriding volcanic arc did not accrete crust");

    Vec3d weathering_direction{};
    bool found_weathering_interior=false;
    for (const TectonicPlate& plate:tectonics.plates()) {
        const TectonicSample sample=tectonics.sample_direction(
            plate.seed_direction
        );
        if (sample.divergence_forcing<=1.0e-12) {
            weathering_direction=plate.seed_direction;
            found_weathering_interior=true;
            break;
        }
    }
    check(
        found_weathering_interior,
        "regolith production test found no non-rifting plate interior"
    );

    GeologyState thin_cover=continent;
    GeologyState thick_cover=continent;
    thin_cover.continental_fraction=1.0;
    thick_cover.continental_fraction=1.0;
    thin_cover.regolith_thickness_m=0.10;
    thick_cover.regolith_thickness_m=3.0;
    geology.advance_tectonics(
        thin_cover,
        weathering_direction,
        100'000.0
    );
    geology.advance_tectonics(
        thick_cover,
        weathering_direction,
        100'000.0
    );
    check(
        thin_cover.regolith_thickness_m-0.10>
        4.0*(thick_cover.regolith_thickness_m-3.0),
        "weathering advanced thin and thick regolith at nearly the same rate"
    );

    GeologyState oceanic_cover=continent;
    oceanic_cover.continental_fraction=0.0;
    oceanic_cover.regolith_thickness_m=0.5;
    geology.advance_tectonics(
        oceanic_cover,
        weathering_direction,
        100'000.0
    );
    near(
        oceanic_cover.regolith_thickness_m,
        0.5,
        1.0e-15,
        "zero-continental column produced regolith"
    );

    GeologyState one_step=continent;
    GeologyState many_steps=continent;
    one_step.continental_fraction=1.0;
    many_steps.continental_fraction=1.0;
    one_step.regolith_thickness_m=0.25;
    many_steps.regolith_thickness_m=0.25;
    geology.advance_tectonics(
        one_step,
        weathering_direction,
        250'000.0
    );
    for (int i=0;i<250;++i) {
        geology.advance_tectonics(
            many_steps,
            weathering_direction,
            1'000.0
        );
    }
    near(
        one_step.regolith_thickness_m,
        many_steps.regolith_thickness_m,
        1.0e-12,
        "analytical regolith production changed with geology timestep size"
    );

    GeologyState source=continent;
    GeologyState sink=ocean;
    source.sediment_mass_kg=2.0e13;
    sink.sediment_mass_kg=3.0e13;
    const double solid_before=
        source.crust_thickness_m*area_m2*source.crust_density_kg_m3+
        source.sediment_mass_kg+
        sink.crust_thickness_m*area_m2*sink.crust_density_kg_m3+
        sink.sediment_mass_kg;
    const ErosionBudget budget=geology.erode(source,area_m2,2.0);
    geology.deposit(sink,budget.transported_mass_kg());

    GeologyState basin=ocean;
    basin.sediment_mass_kg=0.0;
    const double basin_before=geology.surface_elevation_m(
        basin,
        most_oceanic,
        area_m2
    );
    geology.deposit(
        basin,
        geology.sediment_mass_for_thickness_kg(100.0,area_m2)
    );
    const double basin_after=geology.surface_elevation_m(
        basin,
        most_oceanic,
        area_m2
    );
    check(basin_after>basin_before,
          "sediment deposition lowered the basin surface");

    const double solid_after=
        source.crust_thickness_m*area_m2*source.crust_density_kg_m3+
        source.sediment_mass_kg+
        sink.crust_thickness_m*area_m2*sink.crust_density_kg_m3+
        sink.sediment_mass_kg;
    near(solid_after,solid_before,1.0e-13,
         "erosion/deposition transport did not conserve solid mass");
    check(geology.erosion_rate_m_per_year(0.10,0.003,2.0)>
          geology.erosion_rate_m_per_year(0.01,0.003,2.0),
          "stream-power erosion does not increase with slope");
    near(geology.erosion_rate_m_per_year(0.0,0.003,2.0),0.0,1.0e-15,
         "flat terrain erodes under slope-driven erosion law");
    near(geology.erosion_rate_m_per_year(0.10,0.0,2.0),0.0,1.0e-15,
         "fluvial incision remained active without runoff");
    check(
        geology.hillslope_transport_rate_m_per_year(0.10,2.0)>0.0,
        "soil-mantled slope lost water-independent hillslope creep"
    );
    near(
        geology.hillslope_transport_rate_m_per_year(0.10,0.0),
        0.0,
        1.0e-15,
        "bare slope transported nonexistent mobile regolith"
    );
    const double gentle_hillslope_rate=
        geology.hillslope_transport_rate_m_per_year(0.10,2.0);
    const double moderate_hillslope_rate=
        geology.hillslope_transport_rate_m_per_year(0.20,2.0);
    const double near_critical_hillslope_rate=
        geology.hillslope_transport_rate_m_per_year(0.55,2.0);
    const double supercritical_hillslope_rate=
        geology.hillslope_transport_rate_m_per_year(1.0,2.0);

    check(
        moderate_hillslope_rate>gentle_hillslope_rate &&
        moderate_hillslope_rate<2.5*gentle_hillslope_rate,
        "low-gradient hillslope transport lost creep-like scaling"
    );
    check(
        near_critical_hillslope_rate>20.0*gentle_hillslope_rate,
        "hillslope transport did not accelerate near critical slope"
    );
    check(
        std::isfinite(supercritical_hillslope_rate) &&
        supercritical_hillslope_rate>0.0 &&
        supercritical_hillslope_rate<=2.0e-2,
        "critical-slope transport regularization is not finite and bounded"
    );

    const double shallow_marine_rate=
        geology.marine_sediment_transport_rate_m_per_year(
            0.02,
            100.0,
            100.0
        );
    const double deep_marine_rate=
        geology.marine_sediment_transport_rate_m_per_year(
            0.02,
            3'000.0,
            100.0
        );
    check(
        shallow_marine_rate>5.0*deep_marine_rate,
        "marine sediment transport did not weaken into deep water"
    );
    check(
        geology.marine_sediment_transport_rate_m_per_year(
            0.04,
            100.0,
            100.0
        )>shallow_marine_rate,
        "marine sediment transport did not increase with downhill slope"
    );
    near(
        geology.marine_sediment_transport_rate_m_per_year(
            0.02,
            0.0,
            100.0
        ),
        0.0,
        1.0e-15,
        "marine sediment transport remained active above sea level"
    );
    near(
        geology.marine_sediment_transport_rate_m_per_year(
            0.02,
            100.0,
            0.0
        ),
        0.0,
        1.0e-15,
        "marine sediment transport entrained nonexistent sediment"
    );

    GeologyState marine_entrainment=ocean;
    marine_entrainment.regolith_thickness_m=7.0;
    marine_entrainment.sediment_mass_kg=
        geology.sediment_mass_for_thickness_kg(1.0,area_m2);
    const double marine_crust_before=
        marine_entrainment.crust_thickness_m;
    const double marine_regolith_before=
        marine_entrainment.regolith_thickness_m;
    const double entrained_mass=geology.entrain_sediment(
        marine_entrainment,
        area_m2,
        2.0
    );
    check(
        entrained_mass>0.0 &&
        marine_entrainment.sediment_mass_kg<=1.0,
        "marine entrainment did not exhaust a thinner sediment column"
    );
    near(
        marine_entrainment.crust_thickness_m,
        marine_crust_before,
        1.0e-15,
        "marine sediment entrainment eroded bedrock"
    );
    near(
        marine_entrainment.regolith_thickness_m,
        marine_regolith_before,
        1.0e-15,
        "marine sediment entrainment consumed terrestrial regolith state"
    );
}

void test_geology_hillslope_transport_without_runoff() {
    SimulationConfig cfg;
    cfg.base_level=2;
    cfg.max_level=2;
    cfg.tick_seconds=365.2422*86'400.0;

    Simulation sim(42,cfg);
    sim.add_module(std::make_unique<GeographyModule>());
    sim.add_module(std::make_unique<ZeroRunoffModule>());
    sim.build();

    const auto sediment=*sim.fields().find("geology.sediment_mass_kg");
    const auto regolith=*sim.fields().find("geology.regolith_thickness_m");
    const auto erosion_rate=*sim.fields().find("geology.erosion_rate_m_yr");
    const auto runoff=*sim.fields().find("hydrology.runoff_m3_day");
    auto& fs=sim.world().stores().get<FieldStore>();
    const GeologyModel geology(sim.world().seed());

    std::map<CellId,double> before;
    double total_before=0.0;
    for (CellId cell:sim.world().active_cells()) {
        const double area=sim.world().topology().area_m2(cell);
        const double mass=geology.sediment_mass_for_thickness_kg(
            50.0,
            area
        );
        fs.set(cell,sediment,mass);
        fs.set(cell,regolith,2.0);
        near(
            fs.get(cell,runoff),
            0.0,
            1.0e-15,
            "zero-runoff test module initialized nonzero runoff"
        );
        before[cell]=mass;
        total_before+=mass;
    }

    // Geology runs every 24 base ticks. With one simulated year per base tick
    // this executes one 24-year dry hillslope transport step.
    sim.step(24);

    double total_after=0.0;
    double max_change=0.0;
    bool saw_creep=false;
    for (CellId cell:sim.world().active_cells()) {
        const double mass=fs.get(cell,sediment);
        total_after+=mass;
        max_change=std::max(
            max_change,
            std::abs(mass-before.at(cell))
        );
        if (fs.get(cell,erosion_rate)>0.0) saw_creep=true;
        near(
            fs.get(cell,runoff),
            0.0,
            1.0e-15,
            "zero-runoff field changed during geology-only simulation"
        );
    }

    near(
        total_after,
        total_before,
        1.0e-12,
        "dry hillslope transport did not conserve sediment mass"
    );
    check(
        saw_creep && max_change>1.0,
        "zero-runoff hillslope creep was not wired into geology evolution"
    );
}

void test_geology_marine_sediment_transport_without_runoff() {
    SimulationConfig cfg;
    cfg.base_level=2;
    cfg.max_level=2;
    cfg.tick_seconds=365.2422*86'400.0;

    Simulation sim(42,cfg);
    sim.add_module(std::make_unique<GeographyModule>());
    sim.add_module(std::make_unique<ZeroRunoffModule>());
    sim.build();

    const auto sediment=*sim.fields().find("geology.sediment_mass_kg");
    const auto regolith=*sim.fields().find("geology.regolith_thickness_m");
    const auto continental=*sim.fields().find("geology.continental_fraction");
    const auto runoff=*sim.fields().find("hydrology.runoff_m3_day");
    auto& fs=sim.world().stores().get<FieldStore>();

    std::map<CellId,double> before;
    double total_before=0.0;
    for (CellId cell:sim.world().active_cells()) {
        // Force a water-only process fixture. With no runoff, no continental
        // regolith production and no mobile regolith, the pre-marine-routing
        // model had no mechanism capable of changing sediment mass.
        fs.set(cell,continental,0.0);
        fs.set(cell,regolith,0.0);
        near(
            fs.get(cell,runoff),
            0.0,
            1.0e-15,
            "marine-routing fixture initialized nonzero runoff"
        );
        const double mass=fs.get(cell,sediment);
        before[cell]=mass;
        total_before+=mass;
    }

    // Geology runs every 24 base ticks. With one simulated year per base tick
    // this executes one 24-year submerged sediment-routing step.
    sim.step(24);

    double total_after=0.0;
    double max_change=0.0;
    bool saw_transport_rate=false;
    const auto erosion_rate=*sim.fields().find("geology.erosion_rate_m_yr");
    for (CellId cell:sim.world().active_cells()) {
        const double mass=fs.get(cell,sediment);
        total_after+=mass;
        max_change=std::max(
            max_change,
            std::abs(mass-before.at(cell))
        );
        if (fs.get(cell,erosion_rate)>0.0)
            saw_transport_rate=true;
        near(
            fs.get(cell,runoff),
            0.0,
            1.0e-15,
            "marine-routing fixture acquired runoff"
        );
    }

    near(
        total_after,
        total_before,
        1.0e-12,
        "marine sediment routing did not conserve sediment mass"
    );
    check(
        saw_transport_rate && max_change>1.0,
        "submerged sediment did not move without runoff or regolith"
    );
}

void test_geology_mixed_margin_uses_both_crust_sides() {
    constexpr double area_m2=1.0e10;
    constexpr int sample_count=2'048;
    constexpr std::uint64_t seed_count=64;
    constexpr double golden_angle_rad=2.3999632297286533222;
    constexpr double crust_probe_offset_rad=4.0*kPi/180.0;
    constexpr double trench_probe_offset_rad=0.2*kPi/180.0;

    bool found_mixed_margin=false;
    for (
        std::uint64_t seed=0;
        seed<seed_count && !found_mixed_margin;
        ++seed
    ) {
        const GeologyModel geology(seed);
        const TectonicModel& tectonics=geology.tectonics();
        for (int i=0;i<sample_count && !found_mixed_margin;++i) {
            const double z=1.0-2.0*(static_cast<double>(i)+0.5)/
                static_cast<double>(sample_count);
            const double phi=0.413+
                static_cast<double>(i)*golden_angle_rad;
            const double radial=std::sqrt(std::max(0.0,1.0-z*z));
            const Vec3d probe{
                radial*std::cos(phi),
                radial*std::sin(phi),
                z
            };
            const TectonicSample sample=tectonics.sample_direction(probe);
            if (sample.convergence<0.15) continue;

            const Vec3d normal=normalized(
                tectonics.plates()[sample.plate_id].seed_direction-
                tectonics.plates()[sample.neighbor_plate_id].seed_direction
            );
            const Vec3d boundary=normalized(
                probe-normal*dot(probe,normal)
            );
            const Vec3d owner_side=normalized(
                boundary*std::cos(crust_probe_offset_rad)+
                normal*std::sin(crust_probe_offset_rad)
            );
            const Vec3d neighbor_side=normalized(
                boundary*std::cos(crust_probe_offset_rad)-
                normal*std::sin(crust_probe_offset_rad)
            );
            const TectonicSample owner_sample=
                tectonics.sample_direction(owner_side);
            const TectonicSample neighbor_sample=
                tectonics.sample_direction(neighbor_side);
            if (
                owner_sample.plate_id!=sample.plate_id ||
                owner_sample.neighbor_plate_id!=sample.neighbor_plate_id ||
                neighbor_sample.plate_id!=sample.neighbor_plate_id ||
                neighbor_sample.neighbor_plate_id!=sample.plate_id
            ) {
                continue;
            }

            Vec3d continental_side{};
            Vec3d oceanic_side{};
            Vec3d oceanic_normal{};
            if (
                owner_sample.continental_affinity>0.75 &&
                neighbor_sample.continental_affinity<0.45
            ) {
                continental_side=owner_side;
                oceanic_side=neighbor_side;
                oceanic_normal=normal*(-1.0);
            } else if (
                neighbor_sample.continental_affinity>0.75 &&
                owner_sample.continental_affinity<0.45
            ) {
                continental_side=neighbor_side;
                oceanic_side=owner_side;
                oceanic_normal=normal;
            } else {
                continue;
            }

            GeologyState continental=
                geology.initial_state(continental_side,area_m2);
            continental.continental_fraction=0.95;
            const BoundaryFeatureSample continental_features=
                geology.boundary_features(continental,continental_side);

            const Vec3d oceanic_trench=normalized(
                boundary*std::cos(trench_probe_offset_rad)+
                oceanic_normal*std::sin(trench_probe_offset_rad)
            );
            GeologyState oceanic=
                geology.initial_state(oceanic_trench,area_m2);
            oceanic.continental_fraction=0.05;
            const BoundaryFeatureSample oceanic_features=
                geology.boundary_features(oceanic,oceanic_trench);

            check(
                continental_features.volcanic_arc_forcing>0.02,
                "mixed ocean-continent margin lost overriding volcanic arc"
            );
            check(
                continental_features.collision_forcing<0.02,
                "mixed ocean-continent margin was misclassified as collision"
            );
            check(
                oceanic_features.trench_forcing>0.02,
                "mixed ocean-continent margin did not subduct oceanic side"
            );
            found_mixed_margin=true;
        }
    }

    check(
        found_mixed_margin,
        "mixed-margin regression found no resolved ocean-continent convergence"
    );
}

void test_geology_drainage_accumulation() {
    SimulationConfig cfg;
    cfg.base_level=2;
    cfg.max_level=2;
    cfg.tick_seconds=3600.0;
    auto sim=make_terrain_simulation(42,cfg);
    sim->step(24);

    const auto drainage_area=*sim->fields().find("geology.drainage_area_m2");
    const auto discharge=*sim->fields().find(
        "geology.drainage_discharge_m3_day"
    );
    const auto land=*sim->fields().find("geography.land_fraction");
    const auto& fs=sim->world().stores().get<FieldStore>();

    bool accumulated_upstream=false;
    for (CellId cell:sim->world().active_cells()) {
        const double local_land_area=
            sim->world().topology().area_m2(cell)*fs.get(cell,land);
        const double catchment=fs.get(cell,drainage_area);
        const double flow=fs.get(cell,discharge);
        check(std::isfinite(catchment) && catchment>=0.0,
              "invalid accumulated drainage area");
        check(std::isfinite(flow) && flow>=0.0,
              "invalid accumulated drainage discharge");
        check(catchment+1.0>=local_land_area,
              "drainage accumulation lost local contributing area");
        if (catchment>local_land_area*1.01+1.0)
            accumulated_upstream=true;
    }
    check(accumulated_upstream,
          "drainage graph did not accumulate any upstream catchment");
}

void test_geography_refinement_preserves_geology_state() {
    SimulationConfig cfg;
    cfg.base_level=4;
    cfg.max_level=5;
    cfg.tick_seconds=3600.0;
    auto sim=make_terrain_simulation(42,cfg);
    const Vec3d focus=TerrainGenerator::projected_to_direction(0.0,0.0);
    const CellId parent=sim->world().topology().from_direction(focus,cfg.base_level);
    const auto children=parent.children();

    const auto crust=*sim->fields().find("geology.crust_thickness_m");
    const auto continental=*sim->fields().find("geology.continental_fraction");
    const auto age=*sim->fields().find("geology.lithosphere_age_ma");
    const auto sediment=*sim->fields().find("geology.sediment_mass_kg");
    const auto elevation=*sim->fields().find("geography.elevation_m");
    auto& before_fields=sim->world().stores().get<FieldStore>();

    constexpr double sentinel_crust=47'321.0;
    constexpr double sentinel_continental=0.731;
    constexpr double sentinel_age=987.6;
    constexpr double sentinel_sediment=1.23456789e18;
    before_fields.set(parent,crust,sentinel_crust);
    before_fields.set(parent,continental,sentinel_continental);
    before_fields.set(parent,age,sentinel_age);
    before_fields.set(parent,sediment,sentinel_sediment);

    sim->set_focus(focus);
    sim->step(1);

    const auto& fields=sim->world().stores().get<FieldStore>();
    double sediment_sum=0.0;
    double min_elevation=std::numeric_limits<double>::infinity();
    double max_elevation=-std::numeric_limits<double>::infinity();
    for (CellId child:children) {
        check(sim->world().active_cells().contains(child),
              "focused geography parent was not refined");
        near(fields.get(child,crust),sentinel_crust,1e-14,
             "LOD refinement regenerated persistent crust state");
        near(fields.get(child,continental),sentinel_continental,1e-14,
             "LOD refinement regenerated continental fraction");
        near(fields.get(child,age),sentinel_age,1e-14,
             "LOD refinement regenerated lithosphere age");
        sediment_sum+=fields.get(child,sediment);
        min_elevation=std::min(min_elevation,fields.get(child,elevation));
        max_elevation=std::max(max_elevation,fields.get(child,elevation));
    }
    near(sediment_sum,sentinel_sediment,1e-12,
         "LOD refinement did not conserve sediment mass");
    check(max_elevation-min_elevation>1.0e-6,
          "derived geography lost subcell spatial detail");
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
        test_adaptive_cover_resolution();
        test_lod_conservation();
        test_lod_hysteresis();
        test_determinism_and_snapshot();
        test_lod_stability_and_snapshot_across_cover_change();
        test_command_routing_across_lod();
        test_columnar_field_store_and_cohort_index();
        test_ecology_invariants();
        test_flora_pft_contracts();
        test_living_soil_ecology_contracts();
        test_tectonic_model_partition_and_determinism();
        test_tectonic_model_multiseed_robustness();
        test_authoritative_terrain_tracks_tectonic_macro_relief();
        test_authoritative_terrain_scale_and_determinism();
        test_visual_orography_has_local_mountain_prominence();
        test_sphere_native_terrain_continuity();
        test_geology_model_process_contracts();
        test_geology_hillslope_transport_without_runoff();
        test_geology_marine_sediment_transport_without_runoff();
        test_geology_mixed_margin_uses_both_crust_sides();
        test_geology_drainage_accumulation();
        test_geography_refinement_preserves_geology_state();
        test_c_api();
        test_module_extension_contract();
        std::cout << "worldsim_tests: OK\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "worldsim_tests: FAIL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

#include "worldsim/simulation.hpp"
#include "worldsim/modules.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <queue>

namespace worldsim {

void Scheduler::add(std::unique_ptr<ISimSystem> system) {
    if (finalized_) throw std::runtime_error("scheduler already finalized");
    const std::string id(system->id());
    if (id.empty() || systems_.contains(id)) throw std::runtime_error("duplicate/empty system id: "+id);
    systems_.emplace(id,std::move(system));
}

bool Scheduler::conflicts(const ISimSystem& a, const ISimSystem& b) const {
    const auto aa=a.access(), bb=b.access();
    auto intersects=[](const std::vector<std::string>& x,const std::vector<std::string>& y) {
        for (const auto& i:x) if (std::find(y.begin(),y.end(),i)!=y.end()) return true;
        return false;
    };
    return intersects(aa.writes,bb.writes)||intersects(aa.writes,bb.reads)||intersects(bb.writes,aa.reads);
}

bool Scheduler::has_path(std::string_view from, std::string_view to, const std::map<std::string,std::set<std::string>>& edges) const {
    std::set<std::string> seen;
    std::vector<std::string> stack{std::string(from)};
    while (!stack.empty()) {
        auto cur=stack.back(); stack.pop_back();
        if (cur==to) return true;
        if (!seen.insert(cur).second) continue;
        if (auto it=edges.find(cur);it!=edges.end()) for (const auto& n:it->second) stack.push_back(n);
    }
    return false;
}

void Scheduler::finalize() {
    std::map<std::string,std::set<std::string>> edges;
    std::map<std::string,std::size_t> indegree;
    for (const auto& [id,s]: systems_) { (void)s; indegree[id]=0; }
    for (const auto& [id,s]: systems_) {
        for (const auto& dep:s->after()) {
            if (!systems_.contains(dep)) throw std::runtime_error("system "+id+" depends on missing system "+dep);
            if (edges[dep].insert(id).second) ++indegree[id];
        }
    }
    for (auto ia=systems_.begin();ia!=systems_.end();++ia) for (auto ib=std::next(ia);ib!=systems_.end();++ib) {
        if (!conflicts(*ia->second,*ib->second)) continue;
        if (!has_path(ia->first,ib->first,edges) && !has_path(ib->first,ia->first,edges))
            throw std::runtime_error("unordered resource conflict between systems "+ia->first+" and "+ib->first);
    }
    std::priority_queue<std::string,std::vector<std::string>,std::greater<>> ready;
    for (const auto& [id,d]:indegree) if (d==0) ready.push(id);
    order_.clear();
    while (!ready.empty()) {
        auto id=ready.top(); ready.pop();
        order_.push_back(systems_.at(id).get());
        for (const auto& n:edges[id]) if (--indegree[n]==0) ready.push(n);
    }
    if (order_.size()!=systems_.size()) throw std::runtime_error("system dependency cycle");
    finalized_=true;
}

void Scheduler::run(Tick tick, SystemContext& ctx) {
    if (!finalized_) throw std::runtime_error("scheduler not finalized");
    for (ISimSystem* s:order_) {
        const Tick cadence=std::max<Tick>(1,s->cadence_ticks());
        // A cadence-N system integrates after N base ticks have elapsed, not at tick 0.
        // Using the end of the cadence window keeps simulated elapsed time aligned with
        // world.tick() and prevents a daily system from advancing a full day in hour one.
        if ((tick%cadence)==(cadence-1U)) {
            const double old=ctx.dt_days;
            ctx.dt_days=old*static_cast<double>(cadence);
            s->step(ctx);
            ctx.dt_days=old;
        }
    }
}

Simulation::Simulation(std::uint64_t seed, SimulationConfig config): seed_(seed),config_(config) {
    if (config_.base_level>config_.max_level) throw std::invalid_argument("base_level > max_level");
    if (config_.max_level>CellId::kMaxLevel) throw std::invalid_argument("max_level too high");
    if (!(config_.tick_seconds>0.0)) throw std::invalid_argument("tick_seconds must be positive");
}

void Simulation::add_module(std::unique_ptr<ISimModule> module) {
    if (built_) throw std::runtime_error("cannot add module after build");
    modules_.push_back(std::move(module));
}

void Simulation::build() {
    if (built_) throw std::runtime_error("simulation already built");
    std::set<std::string> module_ids;
    for (auto& m:modules_) {
        if (!module_ids.insert(std::string(m->id())).second) throw std::runtime_error("duplicate module id");
        m->register_fields(fields_);
    }
    fields_.freeze();
    world_=std::make_unique<WorldState>(seed_);
    world_->stores().emplace<FieldStore>(fields_);
    for (auto& m:modules_) m->register_stores(world_->stores(),fields_);
    world_->initialize_cover(config_.base_level);
    for (auto& m:modules_) m->register_systems(scheduler_,fields_);
    scheduler_.finalize();
    for (auto& m:modules_) m->initialize(*world_,fields_);
    built_=true;
}

void Simulation::process_commands() {
    if (commands_.empty()) return;
    std::stable_sort(commands_.begin(),commands_.end(),[](const auto& a,const auto& b){
        return a.tick<b.tick || (a.tick==b.tick && a.sequence<b.sequence);
    });
    auto& store=world_->stores().get<FieldStore>();
    std::size_t consumed=0;
    while (consumed<commands_.size() && commands_[consumed].tick<=world_->tick()) {
        const auto& c=commands_[consumed];
        CellId target=c.cell;
        if (!world_->active_cells().contains(target)) {
            // Commands are addressed by spatial region, not by a transient LOD leaf. Resolve
            // the target's center by walking the bounded hierarchy rather than scanning every
            // active leaf; command routing is therefore independent of total active-cell count.
            const Vec3d direction=world_->topology().center_unit(target);
            bool found=false;
            for (std::uint8_t level=config_.base_level; level<=config_.max_level; ++level) {
                const CellId candidate=world_->topology().from_direction(direction,level);
                if (world_->active_cells().contains(candidate)) {
                    target=candidate;
                    found=true;
                    break;
                }
                if (level==config_.max_level) break;
            }
            if (!found) throw std::runtime_error("active spatial cover does not contain command target");
        }
        store.add(target,c.field,c.delta);
        ++consumed;
    }
    commands_.erase(commands_.begin(),commands_.begin()+static_cast<std::ptrdiff_t>(consumed));
}

std::uint8_t Simulation::target_level(Vec3d cell_center, double boundary_margin_deg) const {
    if (!focus_) return config_.base_level;
    const double angle=std::acos(std::clamp(dot(normalized(*focus_),normalized(cell_center)),-1.0,1.0));
    const double deg=angle*180.0/kPi;
    if (deg<12.0+boundary_margin_deg) return config_.max_level;
    if (deg<28.0+boundary_margin_deg && config_.max_level>config_.base_level) return static_cast<std::uint8_t>(config_.max_level-1U);
    if (deg<55.0+boundary_margin_deg && config_.max_level>config_.base_level+1U) return static_cast<std::uint8_t>(config_.max_level-2U);
    return config_.base_level;
}

bool Simulation::update_lod() {
    bool cover_changed=false;
    if (!focus_) {
        for (int level=config_.max_level; level>config_.base_level; --level) {
            std::set<CellId> parents;
            for (CellId c:world_->active_cells()) if (c.level()==level) parents.insert(c.parent());
            for (CellId p:parents) cover_changed=world_->coarsen(p) || cover_changed;
        }
        return cover_changed;
    }

    bool refined=true;
    while (refined) {
        refined=false;
        std::vector<CellId> refine_list;
        for (CellId c:world_->active_cells())
            if (c.level()<target_level(world_->topology().center_unit(c),0.0)) refine_list.push_back(c);
        for (CellId c:refine_list) {
            if (!world_->active_cells().contains(c) || c.level()>=config_.max_level) continue;
            world_->refine(c);
            refined=true;
            cover_changed=true;
        }
    }

    for (int level=config_.max_level;level>config_.base_level;--level) {
        std::set<CellId> parents;
        for (CellId c:world_->active_cells()) if (c.level()==level) parents.insert(c.parent());
        for (CellId p:parents) {
            const auto children=p.children();
            bool all_active=true;
            bool all_want_parent_or_coarser=true;
            for (CellId child:children) {
                if (!world_->active_cells().contains(child)) { all_active=false; break; }
                // Refinement enters at the nominal boundary; coarsening leaves only after
                // moving four degrees beyond it. Authoritative state transitions are much
                // more expensive than render LOD changes, so this hysteresis prevents
                // camera/focus jitter from repeatedly splitting and merging state stores.
                if (target_level(world_->topology().center_unit(child),4.0)>p.level()) {
                    all_want_parent_or_coarser=false;
                    break;
                }
            }
            if (all_active && all_want_parent_or_coarser)
                cover_changed=world_->coarsen(p) || cover_changed;
        }
    }
    return cover_changed;
}

void Simulation::step(Tick ticks) {
    if (!built_) throw std::runtime_error("simulation not built");
    for (Tick i=0;i<ticks;++i) {
        if (update_lod())
            for (auto& module:modules_) module->on_spatial_cover_changed(*world_,fields_);
        process_commands();
        SystemContext ctx{*world_,fields_,config_.tick_seconds/86400.0};
        scheduler_.run(world_->tick(),ctx);
        world_->set_tick(world_->tick()+1);
    }
}

void Simulation::set_focus(Vec3d direction) {
    const double length=std::hypot(direction.x,direction.y,direction.z);
    if (!std::isfinite(length) || !(length>0.0))
        throw std::invalid_argument("focus vector must be finite and non-zero");
    focus_=direction*(1.0/length);
}
void Simulation::clear_focus() { focus_.reset(); }

void Simulation::schedule_field_impulse(Tick tick, CellId cell, FieldId field, double delta) {
    if (!built_) throw std::runtime_error("simulation not built");
    (void)fields_.descriptor(field);
    if (!cell.valid()) throw std::invalid_argument("invalid command cell");
    commands_.push_back({tick,next_sequence_++,cell,field,delta});
}
void Simulation::schedule_field_impulse(Tick tick, CellId cell, std::string_view field_key, double delta) {
    const auto field=fields_.find(field_key);
    if (!field) throw std::invalid_argument("unknown field key");
    schedule_field_impulse(tick,cell,*field,delta);
}

std::vector<std::byte> Simulation::save_snapshot() const {
    if (!built_) throw std::runtime_error("simulation not built");
    BinaryWriter w;
    const std::array<char,8> magic{'W','S','I','M','S','N','A','P'};
    for (char c:magic) w.pod(c);
    w.pod<std::uint32_t>(11); // versioned, little-endian wire format
    w.pod(fields_.schema_hash());
    w.pod(seed_);
    w.pod(config_.base_level);
    w.pod(config_.max_level);
    w.pod(config_.tick_seconds);
    w.pod(world_->tick());

    w.pod<std::uint8_t>(focus_.has_value() ? 1U : 0U);
    if (focus_) { w.pod(focus_->x); w.pod(focus_->y); w.pod(focus_->z); }

    w.pod(next_sequence_);
    w.pod<std::uint64_t>(static_cast<std::uint64_t>(commands_.size()));
    for (const auto& c:commands_) {
        w.pod(c.tick); w.pod(c.sequence); w.pod(c.cell.raw()); w.pod(c.field); w.pod(c.delta);
    }

    w.pod<std::uint64_t>(static_cast<std::uint64_t>(world_->active_cells().size()));
    for (CellId c:world_->active_cells()) w.pod(c.raw());

    const auto& events=world_->pending_events();
    w.pod<std::uint64_t>(static_cast<std::uint64_t>(events.size()));
    for (const auto& e:events) {
        w.pod(e.tick); w.string(e.type); w.pod(e.cell.raw()); w.pod(e.subject); w.pod(e.magnitude);
    }

    w.pod<std::uint32_t>(static_cast<std::uint32_t>(world_->stores().all().size()));
    for (const auto& [key,store]:world_->stores().all()) {
        BinaryWriter sw;
        store->save(sw);
        w.string(key);
        w.pod(store->snapshot_version());
        w.bytes(sw.data());
    }
    return w.take();
}

void Simulation::restore_active_cells(std::vector<CellId> cells) {
    // Normalize the current cover back to the uniform base before reconstructing the snapshot
    // cover. This makes restore independent of the LOD state that existed immediately before load.
    clear_focus();
    for (int level=config_.max_level;level>config_.base_level;--level) {
        std::set<CellId> parents;
        for (CellId c:world_->active_cells()) if (c.level()==level) parents.insert(c.parent());
        for (CellId p:parents) world_->coarsen(p);
    }

    std::set<CellId> desired(cells.begin(),cells.end());
    bool progress=true;
    while (progress) {
        progress=false;
        std::vector<CellId> refine_list;
        for (CellId active:world_->active_cells()) {
            bool has_descendant=false;
            for (CellId d:desired) {
                CellId p=d;
                while (p.level()>active.level()) p=p.parent();
                if (p==active && d!=active) { has_descendant=true; break; }
            }
            if (has_descendant) refine_list.push_back(active);
        }
        for (CellId c:refine_list) { world_->refine(c); progress=true; }
    }
    if (world_->active_cells()!=desired) throw std::runtime_error("snapshot active-cell cover is invalid");
}

void Simulation::load_snapshot(std::span<const std::byte> data) {
    if (!built_) throw std::runtime_error("simulation not built");
    BinaryReader r(data);
    const std::array<char,8> expected{'W','S','I','M','S','N','A','P'};
    for (char c:expected) if (r.pod<char>()!=c) throw std::runtime_error("invalid snapshot magic");
    if (r.pod<std::uint32_t>()!=11) throw std::runtime_error("unsupported snapshot version");
    if (r.pod<std::uint64_t>()!=fields_.schema_hash()) throw std::runtime_error("snapshot field schema mismatch");
    const auto snap_seed=r.pod<std::uint64_t>();
    if (snap_seed!=seed_) throw std::runtime_error("snapshot seed mismatch");
    const auto snap_base=r.pod<std::uint8_t>();
    const auto snap_max=r.pod<std::uint8_t>();
    const auto snap_tick_seconds=r.pod<double>();
    if (snap_base!=config_.base_level || snap_max!=config_.max_level || snap_tick_seconds!=config_.tick_seconds)
        throw std::runtime_error("snapshot simulation config mismatch");
    const Tick snap_tick=r.pod<Tick>();

    std::optional<Vec3d> snap_focus;
    const auto has_focus=r.pod<std::uint8_t>();
    if (has_focus>1U) throw std::runtime_error("invalid snapshot focus flag");
    if (has_focus==1U) {
        Vec3d f{r.pod<double>(),r.pod<double>(),r.pod<double>()};
        if (norm(f)==0.0) throw std::runtime_error("invalid zero snapshot focus");
        snap_focus=f;
    }

    const auto snap_next_sequence=r.pod<std::uint64_t>();
    const auto command_count=r.pod<std::uint64_t>();
    std::vector<FieldImpulseCommand> snap_commands;
    snap_commands.reserve(static_cast<std::size_t>(command_count));
    for (std::uint64_t i=0;i<command_count;++i) {
        FieldImpulseCommand c;
        c.tick=r.pod<Tick>(); c.sequence=r.pod<std::uint64_t>(); c.cell=CellId(r.pod<std::uint64_t>());
        c.field=r.pod<FieldId>(); c.delta=r.pod<double>();
        if (!c.cell.valid()) throw std::runtime_error("snapshot contains invalid command cell");
        (void)fields_.descriptor(c.field);
        snap_commands.push_back(c);
    }

    const auto cell_count=r.pod<std::uint64_t>();
    std::vector<CellId> cells; cells.reserve(static_cast<std::size_t>(cell_count));
    for (std::uint64_t i=0;i<cell_count;++i) {
        CellId cell(r.pod<std::uint64_t>());
        if (!cell.valid() || cell.level()<config_.base_level || cell.level()>config_.max_level)
            throw std::runtime_error("snapshot contains invalid active cell");
        cells.push_back(cell);
    }

    const auto event_count=r.pod<std::uint64_t>();
    std::vector<SimulationEvent> snap_events; snap_events.reserve(static_cast<std::size_t>(event_count));
    for (std::uint64_t i=0;i<event_count;++i) {
        SimulationEvent e;
        e.tick=r.pod<Tick>(); e.type=r.string(); e.cell=CellId(r.pod<std::uint64_t>());
        e.subject=r.pod<std::uint64_t>(); e.magnitude=r.pod<double>();
        if (!e.cell.valid()) throw std::runtime_error("snapshot contains invalid event cell");
        snap_events.push_back(std::move(e));
    }

    const auto store_count=r.pod<std::uint32_t>();
    struct StoreChunk { std::uint32_t version{}; std::vector<std::byte> payload; };
    std::map<std::string,StoreChunk> chunks;
    for (std::uint32_t i=0;i<store_count;++i) {
        const std::string key=r.string();
        const auto version=r.pod<std::uint32_t>();
        auto payload=r.bytes();
        if (!chunks.emplace(key,StoreChunk{version,std::move(payload)}).second)
            throw std::runtime_error("duplicate snapshot store chunk: "+key);
    }
    if (r.remaining()!=0) throw std::runtime_error("snapshot has trailing bytes");
    if (chunks.size()!=world_->stores().all().size()) throw std::runtime_error("snapshot state-store set mismatch");
    for (const auto& [key,store]:world_->stores().all()) {
        (void)store;
        if (!chunks.contains(key)) throw std::runtime_error("snapshot missing store: "+key);
    }
    if (std::set<CellId>(cells.begin(),cells.end()).size()!=cells.size())
        throw std::runtime_error("snapshot contains duplicate active cells");

    // Reconstruct the legal adaptive cover while the current stores still match the current cover,
    // then install the exact serialized store payloads.
    restore_active_cells(cells);
    for (const auto& [key,store]:world_->stores().all()) {
        const auto& chunk=chunks.at(key);
        BinaryReader sr(chunk.payload);
        store->load(sr,chunk.version);
        if (sr.remaining()!=0) throw std::runtime_error("snapshot store chunk has trailing bytes: "+key);
    }
    for (const auto& [key,store]:world_->stores().all()) {
        (void)key;
        store->validate_active_cover(world_->active_cells());
    }

    world_->set_tick(snap_tick);
    world_->replace_pending_events(std::move(snap_events));
    focus_=snap_focus;
    commands_=std::move(snap_commands);
    next_sequence_=snap_next_sequence;
}

std::unique_ptr<Simulation> make_default_simulation(std::uint64_t seed, SimulationConfig config) {
    auto sim=std::make_unique<Simulation>(seed,config);
    sim->add_module(std::make_unique<GeographyModule>());
    sim->add_module(std::make_unique<ClimateModule>());
    sim->add_module(std::make_unique<MagicModule>());
    sim->add_module(std::make_unique<EcologyModule>());
    sim->build();
    return sim;
}

std::unique_ptr<Simulation> make_terrain_simulation(std::uint64_t seed, SimulationConfig config) {
    auto sim=std::make_unique<Simulation>(seed,config);
    sim->add_module(std::make_unique<GeographyModule>());
    sim->build();
    return sim;
}

} // namespace worldsim

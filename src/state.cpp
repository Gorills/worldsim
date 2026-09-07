#include "worldsim/state.hpp"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <map>

namespace worldsim {

void BinaryWriter::string(std::string_view s) {
    pod<std::uint32_t>(static_cast<std::uint32_t>(s.size()));
    const auto* p=reinterpret_cast<const std::byte*>(s.data());
    bytes_.insert(bytes_.end(),p,p+s.size());
}

void BinaryWriter::bytes(std::span<const std::byte> b) {
    pod<std::uint64_t>(static_cast<std::uint64_t>(b.size()));
    bytes_.insert(bytes_.end(),b.begin(),b.end());
}

std::string BinaryReader::string() {
    const auto n=pod<std::uint32_t>();
    if (remaining()<n) throw std::runtime_error("snapshot truncated string");
    const char* p=reinterpret_cast<const char*>(data_.data()+offset_);
    std::string out(p,p+n);
    offset_+=n;
    return out;
}

std::vector<std::byte> BinaryReader::bytes() {
    const auto n=pod<std::uint64_t>();
    if (remaining()<n) throw std::runtime_error("snapshot truncated bytes");
    std::vector<std::byte> out(data_.begin()+static_cast<std::ptrdiff_t>(offset_),
                               data_.begin()+static_cast<std::ptrdiff_t>(offset_+n));
    offset_+=static_cast<std::size_t>(n);
    return out;
}

void CohortStore::on_remove_cell(CellId cell) {
    const auto [first,last]=by_cell_.equal_range(cell);
    std::vector<std::uint64_t> ids;
    for (auto it=first;it!=last;++it) ids.push_back(it->second);
    for (std::uint64_t id:ids) cohorts_.erase(id);
    by_cell_.erase(first,last);
}

void CohortStore::on_refine(CellId parent, std::span<const CellId> children, const CubeSphereTopology& topology) {
    std::vector<Cohort> source;
    const auto [first,last]=by_cell_.equal_range(parent);
    std::vector<std::uint64_t> ids;
    for (auto it=first;it!=last;++it) ids.push_back(it->second);
    for (std::uint64_t id:ids) {
        const auto it=cohorts_.find(id);
        if (it!=cohorts_.end()) { source.push_back(it->second); cohorts_.erase(it); }
    }
    by_cell_.erase(first,last);

    const double parent_area=topology.area_m2(parent);
    for (const auto& c:source) {
        for (CellId child:children) {
            Cohort split=c;
            split.id=next_id_++;
            split.cell=child;
            split.count=c.count*(topology.area_m2(child)/parent_area);
            cohorts_.emplace(split.id,split);
            by_cell_.emplace(split.cell,split.id);
        }
    }
}

void CohortStore::on_coarsen(std::span<const CellId> children, CellId parent, const CubeSphereTopology&) {
    std::map<std::tuple<std::uint64_t,std::uint32_t,std::uint32_t>,std::vector<Cohort>> groups;
    for (CellId child:children) {
        const auto [first,last]=by_cell_.equal_range(child);
        std::vector<std::uint64_t> ids;
        for (auto it=first;it!=last;++it) ids.push_back(it->second);
        for (std::uint64_t id:ids) {
            const auto it=cohorts_.find(id);
            if (it==cohorts_.end()) continue;
            const auto key=std::make_tuple(it->second.lineage_id,it->second.species_id,it->second.functional_group);
            groups[key].push_back(it->second);
            cohorts_.erase(it);
        }
        by_cell_.erase(first,last);
    }
    for (auto& [key,parts]:groups) {
        (void)key;
        Cohort merged=parts.front();
        merged.id=next_id_++;
        merged.cell=parent;
        double count=0.0, body_weighted=0.0, reserve_weighted=0.0;
        for (const auto& p:parts) {
            count+=p.count;
            body_weighted+=p.body_mass_kg*p.count;
            reserve_weighted+=p.reserve_kg*p.count;
        }
        merged.count=count;
        if (count>0.0) {
            merged.body_mass_kg=body_weighted/count;
            merged.reserve_kg=reserve_weighted/count;
        }
        cohorts_.emplace(merged.id,merged);
        by_cell_.emplace(parent,merged.id);
    }
}

void CohortStore::save(BinaryWriter& writer) const {
    writer.pod(next_id_);
    writer.pod<std::uint64_t>(static_cast<std::uint64_t>(cohorts_.size()));
    for (const auto& [id,c]:cohorts_) {
        (void)id;
        writer.pod(c.id); writer.pod(c.lineage_id); writer.pod(c.cell.raw());
        writer.pod(c.species_id); writer.pod(c.functional_group);
        writer.pod(c.count); writer.pod(c.body_mass_kg); writer.pod(c.reserve_kg);
    }
}

void CohortStore::load(BinaryReader& reader, std::uint32_t version) {
    if (version!=1) throw std::runtime_error("unsupported ecology.cohorts snapshot version");
    cohorts_.clear();
    by_cell_.clear();
    next_id_=reader.pod<std::uint64_t>();
    const auto n=reader.pod<std::uint64_t>();
    for (std::uint64_t i=0;i<n;++i) {
        Cohort c;
        c.id=reader.pod<std::uint64_t>(); c.lineage_id=reader.pod<std::uint64_t>();
        c.cell=CellId(reader.pod<std::uint64_t>());
        c.species_id=reader.pod<std::uint32_t>(); c.functional_group=reader.pod<std::uint32_t>();
        c.count=reader.pod<double>(); c.body_mass_kg=reader.pod<double>(); c.reserve_kg=reader.pod<double>();
        if (!c.cell.valid()) throw std::runtime_error("cohort snapshot contains invalid cell");
        if (!std::isfinite(c.count) || !std::isfinite(c.body_mass_kg) || !std::isfinite(c.reserve_kg) ||
            c.count<0.0 || c.body_mass_kg<0.0 || c.reserve_kg<0.0)
            throw std::runtime_error("cohort snapshot contains invalid numeric state");
        if (!cohorts_.emplace(c.id,c).second) throw std::runtime_error("cohort snapshot contains duplicate id");
        by_cell_.emplace(c.cell,c.id);
    }
    if (!cohorts_.empty() && next_id_<=cohorts_.rbegin()->first)
        throw std::runtime_error("cohort snapshot next id is not monotonic");
}

void CohortStore::validate_active_cover(const std::set<CellId>& active_cells) const {
    for (const auto& [id,c]:cohorts_) {
        (void)id;
        if (!active_cells.contains(c.cell)) throw std::runtime_error("cohort references inactive spatial cell");
    }
}

Cohort& CohortStore::add(Cohort cohort) {
    if (cohort.id==0) cohort.id=next_id_++;
    else next_id_=std::max(next_id_,cohort.id+1);
    if (cohort.lineage_id==0) cohort.lineage_id=cohort.id;
    auto [it,ok]=cohorts_.emplace(cohort.id,cohort);
    if (!ok) throw std::runtime_error("duplicate cohort id");
    by_cell_.emplace(it->second.cell,it->first);
    return it->second;
}

std::vector<std::reference_wrapper<Cohort>> CohortStore::in_cell(CellId cell) {
    std::vector<std::reference_wrapper<Cohort>> out;
    const auto [first,last]=by_cell_.equal_range(cell);
    for (auto it=first;it!=last;++it) out.emplace_back(cohorts_.at(it->second));
    return out;
}

std::vector<std::reference_wrapper<const Cohort>> CohortStore::in_cell(CellId cell) const {
    std::vector<std::reference_wrapper<const Cohort>> out;
    const auto [first,last]=by_cell_.equal_range(cell);
    for (auto it=first;it!=last;++it) out.emplace_back(cohorts_.at(it->second));
    return out;
}

double CohortStore::total_count() const {
    double total=0.0;
    for (const auto& [id,c]: cohorts_) { (void)id; total+=c.count; }
    return total;
}

void StateStoreRegistry::add_cell(CellId cell) { for (auto& [k,s]: stores_) { (void)k; s->on_add_cell(cell); } }
void StateStoreRegistry::remove_cell(CellId cell) { for (auto& [k,s]: stores_) { (void)k; s->on_remove_cell(cell); } }
void StateStoreRegistry::refine(CellId parent, std::span<const CellId> children, const CubeSphereTopology& topology) {
    for (auto& [k,s]: stores_) { (void)k; s->on_refine(parent,children,topology); }
}
void StateStoreRegistry::coarsen(std::span<const CellId> children, CellId parent, const CubeSphereTopology& topology) {
    for (auto& [k,s]: stores_) { (void)k; s->on_coarsen(children,parent,topology); }
}

WorldState::WorldState(std::uint64_t seed): seed_(seed) {}

void WorldState::initialize_cover(std::uint8_t level) {
    if (!active_cells_.empty()) throw std::runtime_error("world cover already initialized");
    active_cells_=uniform_cover(level);
    for (CellId c: active_cells_) stores_.add_cell(c);
}

void WorldState::refine(CellId cell) {
    if (!active_cells_.contains(cell)) throw std::runtime_error("cannot refine inactive cell");
    const auto children=cell.children();
    stores_.refine(cell,children,topology_);
    active_cells_.erase(cell);
    active_cells_.insert(children.begin(),children.end());
}

bool WorldState::coarsen(CellId parent) {
    const auto children=parent.children();
    for (CellId c: children) if (!active_cells_.contains(c)) return false;
    stores_.coarsen(children,parent,topology_);
    for (CellId c: children) active_cells_.erase(c);
    active_cells_.insert(parent);
    return true;
}

std::vector<SimulationEvent> WorldState::drain_events() {
    auto out=std::move(events_);
    events_.clear();
    return out;
}

} // namespace worldsim

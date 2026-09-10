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

std::uint64_t CohortStore::transfer_count(
    std::uint64_t cohort_id,
    CellId target,
    double count
) {
    if (!target.valid())
        throw std::invalid_argument("cohort transfer target is invalid");
    if (!std::isfinite(count) || count<0.0)
        throw std::invalid_argument("cohort transfer count is invalid");

    auto source_it=cohorts_.find(cohort_id);
    if (source_it==cohorts_.end())
        throw std::invalid_argument("cohort transfer source is missing");
    Cohort& source=source_it->second;
    if (count>source.count)
        throw std::invalid_argument(
            "cohort transfer exceeds source population"
        );
    if (count==0.0 || target==source.cell)
        return source.id;

    Cohort moved=source;
    moved.cell=target;
    moved.count=count;

    std::uint64_t destination_id=0;
    const auto [first,last]=by_cell_.equal_range(target);
    for (auto it=first;it!=last;++it) {
        Cohort& existing=cohorts_.at(it->second);
        if (
            existing.lineage_id==moved.lineage_id &&
            existing.species_id==moved.species_id &&
            existing.functional_group==moved.functional_group
        ) {
            const double merged_count=existing.count+count;
            if (merged_count>0.0) {
                existing.body_mass_kg=
                    (
                        existing.body_mass_kg*existing.count+
                        moved.body_mass_kg*count
                    )/merged_count;
                existing.reserve_kg=
                    (
                        existing.reserve_kg*existing.count+
                        moved.reserve_kg*count
                    )/merged_count;
            }
            existing.count=merged_count;
            destination_id=existing.id;
            break;
        }
    }

    if (destination_id==0) {
        moved.id=next_id_++;
        const auto [it,ok]=cohorts_.emplace(moved.id,moved);
        if (!ok)
            throw std::runtime_error(
                "cohort transfer generated duplicate id"
            );
        by_cell_.emplace(target,moved.id);
        destination_id=it->first;
    }

    source.count-=count;
    if (source.count<0.0 && source.count>-1.0e-12)
        source.count=0.0;
    return destination_id;
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

std::vector<ActiveCoverPart> WorldState::resolve_active_cover(
    CellId region
) const {
    if (!region.valid())
        throw std::invalid_argument("invalid adaptive-cover region");
    if (active_cells_.empty())
        throw std::runtime_error("adaptive cover is not initialized");
    if (active_cells_.contains(region))
        return {{region,1.0}};

    CellId ancestor=region;
    while (ancestor.level()>0) {
        ancestor=ancestor.parent();
        if (active_cells_.contains(ancestor))
            return {{ancestor,1.0}};
    }

    std::vector<CellId> leaves;
    std::vector<CellId> pending{region};
    while (!pending.empty()) {
        const CellId cell=pending.back();
        pending.pop_back();
        if (active_cells_.contains(cell)) {
            leaves.push_back(cell);
            continue;
        }
        if (cell.level()>=CellId::kMaxLevel)
            throw std::runtime_error("adaptive cover has a spatial hole");
        const auto children=cell.children();
        pending.insert(pending.end(),children.begin(),children.end());
    }
    if (leaves.empty())
        throw std::runtime_error("adaptive cover has a spatial hole");

    std::sort(
        leaves.begin(),
        leaves.end(),
        [](CellId a, CellId b) { return a.raw()<b.raw(); }
    );
    double area_sum=0.0;
    for (CellId leaf:leaves)
        area_sum+=topology_.area_m2(leaf);
    if (!(area_sum>0.0))
        throw std::runtime_error("adaptive cover region has zero area");

    std::vector<ActiveCoverPart> out;
    out.reserve(leaves.size());
    double weight_sum=0.0;
    for (std::size_t i=0;i<leaves.size();++i) {
        const double weight=i+1U==leaves.size()
            ? std::max(0.0,1.0-weight_sum)
            : topology_.area_m2(leaves[i])/area_sum;
        out.push_back({leaves[i],weight});
        weight_sum+=weight;
    }
    if (std::abs(weight_sum-1.0)>1.0e-12)
        throw std::runtime_error(
            "adaptive cover region weights do not close"
        );
    return out;
}

std::array<std::vector<ActiveCoverPart>,4>
WorldState::active_neighbors4(CellId cell) const {
    if (!active_cells_.contains(cell))
        throw std::invalid_argument(
            "active_neighbors4 requires an active source cell"
        );
    std::array<std::vector<ActiveCoverPart>,4> out;
    const auto same_level=topology_.neighbors4(cell);
    for (std::size_t i=0;i<same_level.size();++i)
        out[i]=resolve_active_cover(same_level[i]);
    return out;
}

std::array<std::vector<ActiveFacePart>,4>
WorldState::active_face_neighbors4(CellId cell) const {
    if (!active_cells_.contains(cell))
        throw std::invalid_argument(
            "active_face_neighbors4 requires an active source cell"
        );
    if (const auto cached=active_face_neighbor_cache_.find(cell);
        cached!=active_face_neighbor_cache_.end()) {
        return cached->second;
    }

    constexpr double minimum_interface_length_m=1.0e-6;
    std::array<std::vector<ActiveFacePart>,4> out;
    const auto same_level=topology_.neighbors4(cell);
    for (std::size_t side=0;side<same_level.size();++side) {
        const CellId region=same_level[side];

        CellId ancestor=region;
        bool found_coarse=false;
        while (true) {
            if (active_cells_.contains(ancestor)) {
                const double interface_length=
                    topology_.shared_boundary_length_m(cell,ancestor);
                if (!(interface_length>minimum_interface_length_m))
                    throw std::runtime_error(
                        "active face neighbor has no shared boundary"
                    );
                out[side].push_back({ancestor,interface_length});
                found_coarse=true;
                break;
            }
            if (ancestor.level()==0) break;
            ancestor=ancestor.parent();
        }
        if (found_coarse) continue;

        std::vector<CellId> pending{region};
        while (!pending.empty()) {
            const CellId candidate=pending.back();
            pending.pop_back();
            const double interface_length=
                topology_.shared_boundary_length_m(cell,candidate);
            if (!(interface_length>minimum_interface_length_m))
                continue;
            if (active_cells_.contains(candidate)) {
                out[side].push_back({candidate,interface_length});
                continue;
            }
            if (candidate.level()>=CellId::kMaxLevel)
                throw std::runtime_error(
                    "adaptive face neighborhood has a spatial hole"
                );
            const auto children=candidate.children();
            pending.insert(
                pending.end(),children.begin(),children.end()
            );
        }

        if (out[side].empty())
            throw std::runtime_error(
                "adaptive face neighborhood has a spatial hole"
            );
        std::sort(
            out[side].begin(),
            out[side].end(),
            [](const ActiveFacePart& a, const ActiveFacePart& b) {
                return a.cell.raw()<b.cell.raw();
            }
        );
    }
    active_face_neighbor_cache_[cell]=out;
    return out;
}

void WorldState::initialize_cover(std::uint8_t level) {
    if (!active_cells_.empty()) throw std::runtime_error("world cover already initialized");
    active_face_neighbor_cache_.clear();
    active_cells_=uniform_cover(level);
    for (CellId c: active_cells_) stores_.add_cell(c);
}

void WorldState::refine(CellId cell) {
    if (!active_cells_.contains(cell)) throw std::runtime_error("cannot refine inactive cell");
    active_face_neighbor_cache_.clear();
    const auto children=cell.children();
    stores_.refine(cell,children,topology_);
    active_cells_.erase(cell);
    active_cells_.insert(children.begin(),children.end());
}

bool WorldState::coarsen(CellId parent) {
    const auto children=parent.children();
    for (CellId c: children) if (!active_cells_.contains(c)) return false;
    active_face_neighbor_cache_.clear();
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

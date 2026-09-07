#include "worldsim/fields.hpp"
#include "worldsim/state.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace worldsim {

FieldId FieldRegistry::register_field(FieldDescriptor descriptor) {
    if (frozen_) throw std::runtime_error("field registry is frozen");
    if (descriptor.key.empty()) throw std::invalid_argument("field key is empty");
    if (by_key_.contains(descriptor.key)) throw std::runtime_error("duplicate field: "+descriptor.key);
    if (descriptor.min_value>descriptor.max_value) throw std::invalid_argument("invalid field bounds");
    const FieldId id=static_cast<FieldId>(descriptors_.size());
    by_key_.emplace(descriptor.key,id);
    descriptors_.push_back(std::move(descriptor));
    return id;
}

std::optional<FieldId> FieldRegistry::find(std::string_view key) const {
    const auto it=by_key_.find(key);
    if (it==by_key_.end()) return std::nullopt;
    return it->second;
}

const FieldDescriptor& FieldRegistry::descriptor(FieldId id) const {
    if (static_cast<std::size_t>(id)>=descriptors_.size()) throw std::out_of_range("invalid field id");
    return descriptors_[id];
}

std::uint64_t FieldRegistry::schema_hash() const {
    std::uint64_t h=1469598103934665603ULL;
    for (const auto& d: descriptors_) {
        h^=fnv1a64(d.key); h*=1099511628211ULL;
        h^=fnv1a64(d.unit); h*=1099511628211ULL;
        h^=static_cast<std::uint64_t>(d.semantics); h*=1099511628211ULL;
    }
    return h;
}

FieldStore::FieldStore(const FieldRegistry& registry): registry_(registry), columns_(registry.size()) {
    if (!registry.frozen()) throw std::runtime_error("FieldStore requires a frozen FieldRegistry");
}

double FieldStore::clamp(FieldId field, double value) const {
    const auto& d=registry_.descriptor(field);
    if (!std::isfinite(value)) throw std::runtime_error("non-finite value written to field "+d.key);
    return std::clamp(value,d.min_value,d.max_value);
}

void FieldStore::on_add_cell(CellId cell) {
    if (index_.contains(cell)) throw std::runtime_error("duplicate field cell");
    const std::size_t slot=cells_.size();
    index_.emplace(cell,slot);
    cells_.push_back(cell);
    for (FieldId f=0;f<static_cast<FieldId>(registry_.size());++f)
        columns_[f].push_back(registry_.descriptor(f).default_value);
}

void FieldStore::erase_dense(std::size_t slot) {
    if (slot>=cells_.size()) throw std::out_of_range("invalid dense field index");
    const std::size_t last=cells_.size()-1U;
    const CellId erased=cells_[slot];
    if (slot!=last) {
        const CellId moved=cells_[last];
        cells_[slot]=moved;
        for (auto& column_values:columns_) column_values[slot]=column_values[last];
        index_.at(moved)=slot;
    }
    cells_.pop_back();
    for (auto& column_values:columns_) column_values.pop_back();
    index_.erase(erased);
}

void FieldStore::on_remove_cell(CellId cell) {
    const auto it=index_.find(cell);
    if (it==index_.end()) return;
    erase_dense(it->second);
}

void FieldStore::on_refine(CellId parent, std::span<const CellId> children, const CubeSphereTopology& topology) {
    const auto it=index_.find(parent);
    if (it==index_.end()) throw std::runtime_error("refining missing field row");
    const std::size_t parent_slot=it->second;
    std::vector<double> parent_values(registry_.size());
    for (FieldId f=0;f<static_cast<FieldId>(registry_.size());++f) parent_values[f]=columns_[f][parent_slot];
    const double parent_area=topology.area_m2(parent);
    erase_dense(parent_slot);

    for (CellId child:children) {
        on_add_cell(child);
        const std::size_t slot=dense_index(child);
        const double fraction=topology.area_m2(child)/parent_area;
        for (FieldId f=0;f<static_cast<FieldId>(registry_.size());++f) {
            const double value=registry_.descriptor(f).semantics==FieldSemantics::Extensive
                ? parent_values[f]*fraction : parent_values[f];
            columns_[f][slot]=value;
        }
    }
}

void FieldStore::on_coarsen(std::span<const CellId> children, CellId parent, const CubeSphereTopology& topology) {
    std::vector<double> parent_values(registry_.size(),0.0);
    double area_sum=0.0;
    for (CellId child:children) {
        const auto it=index_.find(child);
        if (it==index_.end()) throw std::runtime_error("coarsening missing field row");
        const std::size_t slot=it->second;
        const double area=topology.area_m2(child);
        area_sum+=area;
        for (FieldId f=0;f<static_cast<FieldId>(registry_.size());++f) {
            const auto sem=registry_.descriptor(f).semantics;
            parent_values[f]+=sem==FieldSemantics::Extensive ? columns_[f][slot] : columns_[f][slot]*area;
        }
    }
    if (!(area_sum>0.0)) throw std::runtime_error("coarsening zero-area cells");
    for (FieldId f=0;f<static_cast<FieldId>(registry_.size());++f)
        if (registry_.descriptor(f).semantics==FieldSemantics::Intensive) parent_values[f]/=area_sum;

    // Removal swaps dense slots, so resolve each child by CellId each time.
    for (CellId child:children) on_remove_cell(child);
    on_add_cell(parent);
    const std::size_t parent_slot=dense_index(parent);
    for (FieldId f=0;f<static_cast<FieldId>(registry_.size());++f) columns_[f][parent_slot]=parent_values[f];
}

std::size_t FieldStore::dense_index(CellId cell) const {
    const auto it=index_.find(cell);
    if (it==index_.end()) throw std::out_of_range("missing cell field row");
    return it->second;
}

double FieldStore::get(CellId cell, FieldId field) const { return get_dense(dense_index(cell),field); }
void FieldStore::set(CellId cell, FieldId field, double value) { set_dense(dense_index(cell),field,value); }
void FieldStore::add(CellId cell, FieldId field, double delta) { set(cell,field,get(cell,field)+delta); }

const std::vector<double>& FieldStore::column(FieldId field) const {
    (void)registry_.descriptor(field);
    return columns_.at(static_cast<std::size_t>(field));
}
std::vector<double>& FieldStore::column(FieldId field) {
    (void)registry_.descriptor(field);
    return columns_.at(static_cast<std::size_t>(field));
}

double FieldStore::get_dense(std::size_t index, FieldId field) const {
    (void)registry_.descriptor(field);
    if (index>=cells_.size()) throw std::out_of_range("invalid dense field index");
    return columns_.at(static_cast<std::size_t>(field))[index];
}
void FieldStore::set_dense(std::size_t index, FieldId field, double value) {
    if (index>=cells_.size()) throw std::out_of_range("invalid dense field index");
    columns_.at(static_cast<std::size_t>(field))[index]=clamp(field,value);
}

void FieldStore::save(BinaryWriter& writer) const {
    // Canonical CellId order makes snapshots independent of dense-slot swap history.
    writer.pod<std::uint64_t>(static_cast<std::uint64_t>(index_.size()));
    for (const auto& [cell,slot]:index_) {
        writer.pod(cell.raw());
        writer.pod<std::uint32_t>(static_cast<std::uint32_t>(registry_.size()));
        for (FieldId f=0;f<static_cast<FieldId>(registry_.size());++f) writer.pod(columns_[f][slot]);
    }
}

void FieldStore::load(BinaryReader& reader, std::uint32_t version) {
    if (version!=1) throw std::runtime_error("unsupported core.fields snapshot version");
    index_.clear();
    cells_.clear();
    columns_.assign(registry_.size(),{});
    const auto count=reader.pod<std::uint64_t>();
    for (std::uint64_t i=0;i<count;++i) {
        CellId cell(reader.pod<std::uint64_t>());
        if (!cell.valid()) throw std::runtime_error("field snapshot contains invalid cell");
        const auto n=reader.pod<std::uint32_t>();
        if (n!=registry_.size()) throw std::runtime_error("field snapshot schema width mismatch");
        on_add_cell(cell);
        const std::size_t slot=dense_index(cell);
        for (FieldId f=0;f<static_cast<FieldId>(registry_.size());++f) {
            const double value=reader.pod<double>();
            const double bounded=clamp(f,value);
            if (bounded!=value) throw std::runtime_error("field snapshot value outside declared bounds");
            columns_[f][slot]=value;
        }
    }
}

void FieldStore::validate_active_cover(const std::set<CellId>& active_cells) const {
    if (active_cells.size()!=index_.size()) throw std::runtime_error("field store active-cover size mismatch");
    for (CellId cell:active_cells) if (!index_.contains(cell))
        throw std::runtime_error("field store missing active cell");
}

} // namespace worldsim

#pragma once

#include "worldsim/spatial.hpp"
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace worldsim {

enum class FieldSemantics : std::uint8_t {
    Intensive = 0,
    Extensive = 1,
};

struct FieldDescriptor {
    std::string key;
    std::string unit;
    FieldSemantics semantics{FieldSemantics::Intensive};
    double default_value{};
    double min_value{-std::numeric_limits<double>::infinity()};
    double max_value{ std::numeric_limits<double>::infinity()};
};

class FieldRegistry {
public:
    FieldId register_field(FieldDescriptor descriptor);
    [[nodiscard]] std::optional<FieldId> find(std::string_view key) const;
    [[nodiscard]] const FieldDescriptor& descriptor(FieldId id) const;
    [[nodiscard]] std::size_t size() const { return descriptors_.size(); }
    void freeze() { frozen_=true; }
    [[nodiscard]] bool frozen() const { return frozen_; }
    [[nodiscard]] std::uint64_t schema_hash() const;
private:
    std::vector<FieldDescriptor> descriptors_;
    std::map<std::string,FieldId,std::less<>> by_key_;
    bool frozen_{};
};

class BinaryWriter;
class BinaryReader;

class IStateStore {
public:
    virtual ~IStateStore()=default;
    [[nodiscard]] virtual std::string_view key() const=0;
    [[nodiscard]] virtual std::uint32_t snapshot_version() const { return 1; }
    virtual void on_add_cell(CellId cell)=0;
    virtual void on_remove_cell(CellId cell)=0;
    virtual void on_refine(CellId parent, std::span<const CellId> children, const CubeSphereTopology& topology)=0;
    virtual void on_coarsen(std::span<const CellId> children, CellId parent, const CubeSphereTopology& topology)=0;
    virtual void save(BinaryWriter& writer) const=0;
    virtual void load(BinaryReader& reader, std::uint32_t version)=0;
    virtual void validate_active_cover(const std::set<CellId>&) const {}
};

class FieldStore final : public IStateStore {
public:
    explicit FieldStore(const FieldRegistry& registry);
    static constexpr std::string_view kKey="core.fields";
    [[nodiscard]] std::string_view key() const override { return kKey; }

    void on_add_cell(CellId cell) override;
    void on_remove_cell(CellId cell) override;
    void on_refine(CellId parent, std::span<const CellId> children, const CubeSphereTopology& topology) override;
    void on_coarsen(std::span<const CellId> children, CellId parent, const CubeSphereTopology& topology) override;
    void save(BinaryWriter& writer) const override;
    void load(BinaryReader& reader, std::uint32_t version) override;
    void validate_active_cover(const std::set<CellId>& active_cells) const override;

    [[nodiscard]] double get(CellId cell, FieldId field) const;
    void set(CellId cell, FieldId field, double value);
    void add(CellId cell, FieldId field, double delta);
    [[nodiscard]] bool contains(CellId cell) const { return index_.contains(cell); }

    [[nodiscard]] const std::vector<CellId>& dense_cells() const { return cells_; }
    [[nodiscard]] const std::vector<double>& column(FieldId field) const;
    [[nodiscard]] std::vector<double>& column(FieldId field);
    [[nodiscard]] std::size_t dense_index(CellId cell) const;
    [[nodiscard]] double get_dense(std::size_t index, FieldId field) const;
    void set_dense(std::size_t index, FieldId field, double value);
private:
    double clamp(FieldId field, double value) const;
    void erase_dense(std::size_t index);

    const FieldRegistry& registry_;
    std::map<CellId,std::size_t> index_;
    std::vector<CellId> cells_;
    std::vector<std::vector<double>> columns_;
};

} // namespace worldsim

#pragma once

#include "worldsim/fields.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace worldsim {

class BinaryWriter {
public:
    // Snapshot primitives are encoded explicitly little-endian; no native struct layouts are serialized.
    template<class T> requires (std::is_integral_v<T> || std::is_floating_point_v<T>)
    void pod(T value) {
        if constexpr (std::is_floating_point_v<T>) {
            static_assert(std::numeric_limits<T>::is_iec559, "WorldSim snapshots require IEEE-754 floating point");
            if constexpr (sizeof(T)==sizeof(std::uint64_t)) {
                write_unsigned(std::bit_cast<std::uint64_t>(value));
            } else if constexpr (sizeof(T)==sizeof(std::uint32_t)) {
                write_unsigned(std::bit_cast<std::uint32_t>(value));
            } else {
                static_assert(sizeof(T)==4 || sizeof(T)==8, "unsupported floating-point width");
            }
        } else {
            using U=std::make_unsigned_t<T>;
            write_unsigned(static_cast<U>(value));
        }
    }
    void string(std::string_view s);
    void bytes(std::span<const std::byte> b);
    [[nodiscard]] const std::vector<std::byte>& data() const { return bytes_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }
private:
    template<class U> requires std::is_unsigned_v<U>
    void write_unsigned(U value) {
        for (std::size_t i=0;i<sizeof(U);++i)
            bytes_.push_back(static_cast<std::byte>((value>>(i*8U)) & static_cast<U>(0xffU)));
    }
    std::vector<std::byte> bytes_;
};

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> data): data_(data) {}
    template<class T> requires (std::is_integral_v<T> || std::is_floating_point_v<T>)
    T pod() {
        if constexpr (std::is_floating_point_v<T>) {
            static_assert(std::numeric_limits<T>::is_iec559, "WorldSim snapshots require IEEE-754 floating point");
            if constexpr (sizeof(T)==sizeof(std::uint64_t)) {
                return std::bit_cast<T>(read_unsigned<std::uint64_t>());
            } else if constexpr (sizeof(T)==sizeof(std::uint32_t)) {
                return std::bit_cast<T>(read_unsigned<std::uint32_t>());
            } else {
                static_assert(sizeof(T)==4 || sizeof(T)==8, "unsupported floating-point width");
            }
        } else {
            using U=std::make_unsigned_t<T>;
            return static_cast<T>(read_unsigned<U>());
        }
    }
    std::string string();
    std::vector<std::byte> bytes();
    [[nodiscard]] std::size_t remaining() const { return data_.size()-offset_; }
private:
    template<class U> requires std::is_unsigned_v<U>
    U read_unsigned() {
        if (offset_+sizeof(U)>data_.size()) throw std::runtime_error("snapshot truncated");
        U value{};
        for (std::size_t i=0;i<sizeof(U);++i) {
            const auto byte=static_cast<U>(std::to_integer<unsigned int>(data_[offset_+i]));
            value|=static_cast<U>(byte<<(i*8U));
        }
        offset_+=sizeof(U);
        return value;
    }
    std::span<const std::byte> data_;
    std::size_t offset_{};
};

struct Cohort {
    std::uint64_t id{};
    std::uint64_t lineage_id{};
    CellId cell;
    std::uint32_t species_id{};
    std::uint32_t functional_group{}; // 1 herbivore, 2 carnivore in demo module
    double count{};
    double body_mass_kg{};
    double reserve_kg{};
};

// Reduced wet-mass-to-carbon conversion used by the fauna material budget.
// body_mass_kg and reserve_kg are per-individual wet masses; population count
// is the only demographic degree of freedom in the current cohort model.
inline constexpr double kFaunaCarbonFractionOfWetMass=0.15;

[[nodiscard]] inline double cohort_carbon_kg(const Cohort& cohort) {
    return cohort.count*
        (cohort.body_mass_kg+cohort.reserve_kg)*
        kFaunaCarbonFractionOfWetMass;
}

class CohortStore final : public IStateStore {
public:
    static constexpr std::string_view kKey="ecology.cohorts";
    [[nodiscard]] std::string_view key() const override { return kKey; }
    void on_add_cell(CellId) override {}
    void on_remove_cell(CellId cell) override;
    void on_refine(CellId parent, std::span<const CellId> children, const CubeSphereTopology& topology) override;
    void on_coarsen(std::span<const CellId> children, CellId parent, const CubeSphereTopology& topology) override;
    void save(BinaryWriter& writer) const override;
    void load(BinaryReader& reader, std::uint32_t version) override;
    void validate_active_cover(const std::set<CellId>& active_cells) const override;

    Cohort& add(Cohort cohort);
    std::uint64_t transfer_count(
        std::uint64_t cohort_id,
        CellId target,
        double count
    );
    [[nodiscard]] std::vector<std::reference_wrapper<Cohort>> in_cell(CellId cell);
    [[nodiscard]] std::vector<std::reference_wrapper<const Cohort>> in_cell(CellId cell) const;
    [[nodiscard]] const std::map<std::uint64_t,Cohort>& all() const { return cohorts_; }
    [[nodiscard]] double total_count() const;
private:
    std::map<std::uint64_t,Cohort> cohorts_;
    std::multimap<CellId,std::uint64_t> by_cell_;
    std::uint64_t next_id_{1};
};

class StateStoreRegistry {
public:
    template<class T, class... Args>
    T& emplace(Args&&... args) {
        static_assert(std::is_base_of_v<IStateStore,T>);
        auto ptr=std::make_unique<T>(std::forward<Args>(args)...);
        const std::string k(ptr->key());
        if (stores_.contains(k)) throw std::runtime_error("duplicate state store: "+k);
        T& ref=*ptr;
        stores_.emplace(k,std::move(ptr));
        return ref;
    }
    template<class T> T& get() {
        auto it=stores_.find(std::string(T::kKey));
        if (it==stores_.end()) throw std::runtime_error("missing state store");
        auto* p=dynamic_cast<T*>(it->second.get());
        if (!p) throw std::runtime_error("state store type mismatch");
        return *p;
    }
    template<class T> const T& get() const {
        auto it=stores_.find(std::string(T::kKey));
        if (it==stores_.end()) throw std::runtime_error("missing state store");
        auto* p=dynamic_cast<const T*>(it->second.get());
        if (!p) throw std::runtime_error("state store type mismatch");
        return *p;
    }
    void add_cell(CellId cell);
    void remove_cell(CellId cell);
    void refine(CellId parent, std::span<const CellId> children, const CubeSphereTopology& topology);
    void coarsen(std::span<const CellId> children, CellId parent, const CubeSphereTopology& topology);
    [[nodiscard]] const std::map<std::string,std::unique_ptr<IStateStore>>& all() const { return stores_; }
private:
    std::map<std::string,std::unique_ptr<IStateStore>> stores_;
};

struct SimulationEvent {
    Tick tick{};
    std::string type;
    CellId cell;
    std::uint64_t subject{};
    double magnitude{};
};

struct ActiveCoverPart {
    CellId cell;
    double weight{};
};

class WorldState {
public:
    explicit WorldState(std::uint64_t seed);
    [[nodiscard]] std::uint64_t seed() const { return seed_; }
    [[nodiscard]] Tick tick() const { return tick_; }
    void set_tick(Tick t) { tick_=t; }
    [[nodiscard]] const CubeSphereTopology& topology() const { return topology_; }
    [[nodiscard]] CubeSphereTopology& topology() { return topology_; }
    [[nodiscard]] const std::set<CellId>& active_cells() const { return active_cells_; }
    [[nodiscard]] std::vector<ActiveCoverPart> resolve_active_cover(
        CellId region
    ) const;
    [[nodiscard]] std::array<std::vector<ActiveCoverPart>,4>
    active_neighbors4(CellId cell) const;
    [[nodiscard]] StateStoreRegistry& stores() { return stores_; }
    [[nodiscard]] const StateStoreRegistry& stores() const { return stores_; }
    void initialize_cover(std::uint8_t level);
    void refine(CellId cell);
    bool coarsen(CellId parent);
    void emit(SimulationEvent event) { events_.push_back(std::move(event)); }
    [[nodiscard]] const std::vector<SimulationEvent>& pending_events() const { return events_; }
    void replace_pending_events(std::vector<SimulationEvent> events) { events_=std::move(events); }
    [[nodiscard]] std::vector<SimulationEvent> drain_events();
private:
    std::uint64_t seed_{};
    Tick tick_{};
    CubeSphereTopology topology_;
    std::set<CellId> active_cells_;
    StateStoreRegistry stores_;
    std::vector<SimulationEvent> events_;
};

} // namespace worldsim

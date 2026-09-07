#pragma once

#include "worldsim/state.hpp"
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <vector>

namespace worldsim {

struct SystemAccess {
    std::vector<std::string> reads;
    std::vector<std::string> writes;
};

struct SystemContext {
    WorldState& world;
    FieldRegistry& fields;
    double dt_days{};
};

class ISimSystem {
public:
    virtual ~ISimSystem()=default;
    [[nodiscard]] virtual std::string_view id() const=0;
    [[nodiscard]] virtual Tick cadence_ticks() const { return 1; }
    [[nodiscard]] virtual std::vector<std::string> after() const { return {}; }
    [[nodiscard]] virtual SystemAccess access() const=0;
    virtual void step(SystemContext& ctx)=0;
};

class Scheduler {
public:
    void add(std::unique_ptr<ISimSystem> system);
    void finalize();
    void run(Tick tick, SystemContext& ctx);
    [[nodiscard]] const std::vector<ISimSystem*>& order() const { return order_; }
private:
    bool conflicts(const ISimSystem& a, const ISimSystem& b) const;
    bool has_path(std::string_view from, std::string_view to, const std::map<std::string,std::set<std::string>>& edges) const;
    std::map<std::string,std::unique_ptr<ISimSystem>> systems_;
    std::vector<ISimSystem*> order_;
    bool finalized_{};
};

class ISimModule {
public:
    virtual ~ISimModule()=default;
    [[nodiscard]] virtual std::string_view id() const=0;
    virtual void register_fields(FieldRegistry&) {}
    virtual void register_stores(StateStoreRegistry&, const FieldRegistry&) {}
    virtual void register_systems(Scheduler&, const FieldRegistry&) {}
    virtual void initialize(WorldState&, const FieldRegistry&) {}
};

struct SimulationConfig {
    std::uint8_t base_level{4};
    std::uint8_t max_level{7};
    double tick_seconds{3600.0};
};

struct FieldImpulseCommand {
    Tick tick{};
    std::uint64_t sequence{};
    CellId cell;
    FieldId field{};
    double delta{};
};

class Simulation {
public:
    Simulation(std::uint64_t seed, SimulationConfig config={});
    void add_module(std::unique_ptr<ISimModule> module);
    void build();
    void step(Tick ticks=1);
    void set_focus(Vec3d direction);
    void clear_focus();
    void schedule_field_impulse(Tick tick, CellId cell, FieldId field, double delta);
    void schedule_field_impulse(Tick tick, CellId cell, std::string_view field_key, double delta);

    [[nodiscard]] WorldState& world() { return *world_; }
    [[nodiscard]] const WorldState& world() const { return *world_; }
    [[nodiscard]] FieldRegistry& fields() { return fields_; }
    [[nodiscard]] const FieldRegistry& fields() const { return fields_; }
    [[nodiscard]] const SimulationConfig& config() const { return config_; }
    [[nodiscard]] bool built() const { return built_; }

    [[nodiscard]] std::vector<std::byte> save_snapshot() const;
    void load_snapshot(std::span<const std::byte> data);
private:
    void process_commands();
    void update_lod();
    std::uint8_t target_level(Vec3d cell_center, double boundary_margin_deg) const;
    void restore_active_cells(std::vector<CellId> cells);

    std::uint64_t seed_{};
    SimulationConfig config_;
    FieldRegistry fields_;
    Scheduler scheduler_;
    std::vector<std::unique_ptr<ISimModule>> modules_;
    std::unique_ptr<WorldState> world_;
    bool built_{};
    std::optional<Vec3d> focus_;
    std::vector<FieldImpulseCommand> commands_;
    std::uint64_t next_sequence_{};
};

std::unique_ptr<Simulation> make_default_simulation(std::uint64_t seed, SimulationConfig config={});

} // namespace worldsim

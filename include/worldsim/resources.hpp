#pragma once

#include "worldsim/climate.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace worldsim {

enum class ResourceKind : std::uint8_t {
    FreshWater = 0,
    PlantFood = 1,
    Wood = 2,
    Stone = 3,
    MetalOre = 4,
    Count = 5,
};

struct ResourceDescriptor {
    ResourceKind kind;
    std::string_view key;
    std::string_view unit;
};

inline constexpr std::array<ResourceDescriptor,5> kResourceDescriptors{{
    {ResourceKind::FreshWater,"fresh_water","L"},
    {ResourceKind::PlantFood,"plant_food","kg"},
    {ResourceKind::Wood,"wood","kg"},
    {ResourceKind::Stone,"stone","kg"},
    {ResourceKind::MetalOre,"metal_ore","kg"},
}};

[[nodiscard]] const ResourceDescriptor& resource_descriptor(ResourceKind kind);
[[nodiscard]] std::optional<ResourceKind> resource_kind_from_key(std::string_view key);

class PlayerInventoryStore final : public IStateStore {
public:
    static constexpr std::string_view kKey="gameplay.player_inventory";
    [[nodiscard]] std::string_view key() const override { return kKey; }

    void on_add_cell(CellId) override {}
    void on_remove_cell(CellId) override {}
    void on_refine(CellId,std::span<const CellId>,const CubeSphereTopology&) override {}
    void on_coarsen(std::span<const CellId>,CellId,const CubeSphereTopology&) override {}
    void save(BinaryWriter& writer) const override;
    void load(BinaryReader& reader,std::uint32_t version) override;

    [[nodiscard]] double amount(ResourceKind kind) const;
    void add(ResourceKind kind,double amount);
private:
    std::array<double,5> amounts_{};
};

class ResourceModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "resources"; }
    void register_fields(FieldRegistry&) override;
    void register_stores(StateStoreRegistry&,const FieldRegistry&) override;
    void register_systems(Scheduler&,const FieldRegistry&) override;
    void initialize(WorldState&,const FieldRegistry&) override;
};

[[nodiscard]] std::unique_ptr<Simulation> make_survival_simulation(
    std::uint64_t seed,
    SimulationConfig config={}
);

[[nodiscard]] double resource_availability(
    const Simulation& simulation,
    Vec3d direction,
    ResourceKind kind
);

// Exact authoritative transfer. Invalid, unavailable and over-large requests
// throw before mutation; successful calls return the requested amount.
double collect_resource(
    Simulation& simulation,
    Vec3d direction,
    ResourceKind kind,
    double requested_amount
);

[[nodiscard]] double player_inventory_amount(
    const Simulation& simulation,
    ResourceKind kind
);

// Natural diagnostics deliberately exclude gameplay inventory. Survival-level
// water accounting includes carried fresh water as a physical stock.
[[nodiscard]] inline double total_survival_planet_water_m3(
    const Simulation& simulation
) {
    return total_planet_water_m3(
        simulation.world(),simulation.fields()
    )+player_inventory_amount(
        simulation,ResourceKind::FreshWater
    )/1000.0;
}

} // namespace worldsim

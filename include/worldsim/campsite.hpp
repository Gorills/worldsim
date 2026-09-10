#pragma once

#include "worldsim/simulation.hpp"

#include <cstdint>
#include <map>

namespace worldsim {

inline constexpr std::uint8_t kGameplaySiteLevel=16;
inline constexpr double kBasicShelterWoodCostKg=6.0;
inline constexpr double kBasicShelterStoneCostKg=2.0;
inline constexpr double kCampfireStoneCostKg=2.0;
inline constexpr double kCampfireBurnKgPerHour=0.25;

struct CampsiteState {
    bool shelter{};
    bool campfire{};
    bool campfire_lit{};
    double campfire_fuel_kg{};
};

class CampsiteStore final : public IStateStore {
public:
    static constexpr std::string_view kKey="gameplay.campsites";
    [[nodiscard]] std::string_view key() const override { return kKey; }

    // Campsite identity uses a fixed gameplay-resolution CellId and is not part
    // of the adaptive simulation cover. LOD changes therefore do not remap it.
    void on_add_cell(CellId) override {}
    void on_remove_cell(CellId) override {}
    void on_refine(CellId,std::span<const CellId>,const CubeSphereTopology&) override {}
    void on_coarsen(std::span<const CellId>,CellId,const CubeSphereTopology&) override {}
    void save(BinaryWriter& writer) const override;
    void load(BinaryReader& reader,std::uint32_t version) override;

    [[nodiscard]] const CampsiteState* find(CellId site) const;
    CampsiteState& ensure(CellId site);
    void burn_lit_campfires(double hours);

private:
    std::map<CellId,CampsiteState> sites_;
};

class CampsiteModule final : public ISimModule {
public:
    [[nodiscard]] std::string_view id() const override { return "campsite"; }
    void register_stores(StateStoreRegistry&,const FieldRegistry&) override;
    void register_systems(Scheduler&,const FieldRegistry&) override;
};

[[nodiscard]] CellId campsite_site_cell(
    const Simulation& simulation,
    Vec3d direction
);

[[nodiscard]] bool campsite_site_suitable(
    const Simulation& simulation,
    Vec3d direction
);

[[nodiscard]] CampsiteState campsite_state(
    const Simulation& simulation,
    Vec3d direction
);

void build_basic_shelter(Simulation& simulation,Vec3d direction);
void build_campfire(Simulation& simulation,Vec3d direction);
void add_campfire_fuel(
    Simulation& simulation,
    Vec3d direction,
    double wood_kg
);
void set_campfire_lit(
    Simulation& simulation,
    Vec3d direction,
    bool lit
);

} // namespace worldsim

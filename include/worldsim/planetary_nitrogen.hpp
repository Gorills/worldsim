#pragma once

#include "worldsim/simulation.hpp"

namespace worldsim {

struct NitrogenBudget {
    double fixed_from_atmosphere_kg{};
    double reactive_deposited_kg{};
    double terrestrial_leached_to_ocean_kg{};
    double fire_emitted_to_atmosphere_kg{};
    double ocean_denitrified_to_atmosphere_kg{};
};

// Reduced global reservoirs that close the terrestrial nitrogen boundary.
// Spatial land stocks remain extensive ecology fields; this store owns only
// non-spatial atmosphere/ocean reservoirs and their cumulative transfer audit.
class NitrogenStore final : public IStateStore {
public:
    static constexpr std::string_view kKey="ecology.nitrogen.state";
    [[nodiscard]] std::string_view key() const override { return kKey; }
    [[nodiscard]] std::uint32_t snapshot_version() const override { return 1; }

    void on_add_cell(CellId) override {}
    void on_remove_cell(CellId) override {}
    void on_refine(
        CellId,
        std::span<const CellId>,
        const CubeSphereTopology&
    ) override {}
    void on_coarsen(
        std::span<const CellId>,
        CellId,
        const CubeSphereTopology&
    ) override {}
    void save(BinaryWriter&) const override;
    void load(BinaryReader&, std::uint32_t version) override;

    void initialize(const WorldState&, const FieldRegistry&);
    void advance(
        WorldState&,
        const FieldRegistry&,
        double terrestrial_leached_kg,
        double fire_emitted_kg,
        double dt_days
    );

    [[nodiscard]] double atmospheric_n2_kg() const {
        return atmospheric_n2_kg_;
    }
    [[nodiscard]] double atmospheric_reactive_nitrogen_kg() const {
        return atmospheric_reactive_nitrogen_kg_;
    }
    [[nodiscard]] double ocean_dissolved_nitrogen_kg() const {
        return ocean_dissolved_nitrogen_kg_;
    }
    [[nodiscard]] const NitrogenBudget& budget() const { return budget_; }

private:
    double atmospheric_n2_kg_{};
    double atmospheric_reactive_nitrogen_kg_{};
    double ocean_dissolved_nitrogen_kg_{};
    NitrogenBudget budget_;
};

[[nodiscard]] double total_planet_nitrogen_kg(
    const WorldState&,
    const FieldRegistry&
);

} // namespace worldsim

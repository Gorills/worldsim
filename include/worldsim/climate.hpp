#pragma once

#include "worldsim/simulation.hpp"

namespace worldsim {

struct ClimateNode {
    CellId cell;
    double area_m2{};
    double land_area_m2{};
    double mean_elevation_m{};
    double land_temperature_k{};
    double ocean_temperature_k{};
    double atmospheric_water_m3{};
    double precipitation_m3_day{};
    double evaporation_m3_day{};
    double solar_flux_w_m2{};
    double net_radiation_w_m2{};
    double relative_humidity{};
    double east_wind_m_s{};
    double north_wind_m_s{};
    double weather_anomaly_k{};
};

struct ClimateBudget {
    double absorbed_solar_j{};
    double outgoing_longwave_j{};
    double land_precipitation_m3{};
    double ocean_precipitation_m3{};
    double land_evaporation_m3{};
    double ocean_evaporation_m3{};
    double terrestrial_ocean_return_m3{};
};

// Persistent energy/moisture reservoirs on the initial uniform cover. Active
// simulation LOD receives projections and never owns the transport graph.
class ClimateStore final : public IStateStore {
public:
    static constexpr std::string_view kKey="climate.state";
    [[nodiscard]] std::string_view key() const override { return kKey; }
    [[nodiscard]] std::uint32_t snapshot_version() const override { return 1; }
    void on_add_cell(CellId cell) override;
    void on_remove_cell(CellId) override {}
    void on_refine(CellId, std::span<const CellId>, const CubeSphereTopology&) override {}
    void on_coarsen(std::span<const CellId>, CellId, const CubeSphereTopology&) override {}
    void save(BinaryWriter&) const override;
    void load(BinaryReader&, std::uint32_t version) override;
    void validate_active_cover(const std::set<CellId>&) const override;

    void initialize(WorldState&, const FieldRegistry&);
    void advance(WorldState&, const FieldRegistry&, double dt_days);
    void exchange_surface(WorldState&, const FieldRegistry&, double dt_days);
    void project(WorldState&, const FieldRegistry&) const;

    [[nodiscard]] const std::vector<ClimateNode>& nodes() const { return nodes_; }
    [[nodiscard]] const ClimateBudget& budget() const { return budget_; }
    [[nodiscard]] double ocean_water_m3() const { return ocean_water_m3_; }
    [[nodiscard]] double total_atmospheric_water_m3() const;
    [[nodiscard]] double total_surface_heat_j() const;
    [[nodiscard]] std::size_t node_index(CellId active_cell) const;

private:
    struct Link {
        std::size_t a{},b{};
        double distance_m{};
        double interface_m{};
        Vec3d tangent_a_to_b;
        Vec3d tangent_b_to_a;
    };

    void rebuild_graph();
    void update_diagnostics(double day);
    void project_surface_exchange(WorldState&, const FieldRegistry&) const;
    void advance_energy(double dt_days);
    void advance_moisture(double dt_days);

    std::uint8_t reference_level_{};
    bool has_level_{};
    std::vector<ClimateNode> nodes_;
    std::map<CellId,std::size_t> index_;
    std::vector<Link> links_;
    ClimateBudget budget_;
    double ocean_water_m3_{};
};

// Coupled default-world inventory. Climate precipitation/evaporation ledgers
// are deliberately excluded because they are flux histories, not stocks.
double total_planet_water_m3(const WorldState&, const FieldRegistry&);

} // namespace worldsim

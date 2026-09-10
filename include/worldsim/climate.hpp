#pragma once

#include "worldsim/simulation.hpp"

namespace worldsim {

// Reduced snow-cover and broadband land-albedo closures used by the coupled
// climate, diagnostics and tests. SWE is liquid-water-equivalent depth.
[[nodiscard]] double snow_cover_fraction_from_swe(double swe_m);
[[nodiscard]] double climate_land_albedo(double snow_cover_fraction);
struct ClimateNode {
    CellId cell;
    double area_m2{};
    double land_area_m2{};
    double mean_elevation_m{};
    double orographic_elevation_m{};
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
    double snow_cover_fraction{};
};

struct ClimateBudget {
    double absorbed_solar_j{};
    double outgoing_longwave_j{};
    double co2_forcing_j{};
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
    [[nodiscard]] std::uint32_t snapshot_version() const override { return 4; }
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
    void advance_carbon(double terrestrial_to_atmosphere_kg, double dt_days);
    void project(WorldState&, const FieldRegistry&) const;
    void project_carbon(WorldState&, const FieldRegistry&) const;

    [[nodiscard]] const std::vector<ClimateNode>& nodes() const { return nodes_; }
    [[nodiscard]] const ClimateBudget& budget() const { return budget_; }
    [[nodiscard]] double ocean_water_m3() const { return ocean_water_m3_; }
    [[nodiscard]] double atmospheric_carbon_kg() const {
        return atmospheric_carbon_kg_;
    }
    [[nodiscard]] double ocean_carbon_kg() const { return ocean_carbon_kg_; }
    [[nodiscard]] double atmospheric_co2_ppm() const;
    [[nodiscard]] double co2_radiative_forcing_w_m2() const;
    [[nodiscard]] double total_atmospheric_water_m3() const;
    [[nodiscard]] double total_surface_heat_j() const;
    [[nodiscard]] std::size_t node_index(CellId active_cell) const;

private:
    struct Link {
        std::size_t a{},b{};
        double distance_m{};
        double interface_m{};
        Vec3d edge_start;
        Vec3d edge_end;
    };

    struct WeatherSample {
        CellId cell;
        double area_weight{};
    };

    void rebuild_graph();
    void rebuild_weather_support();
    void update_snow_cover(const WorldState&, const FieldRegistry&);
    void update_diagnostics(double day);
    void project_surface_exchange(WorldState&, const FieldRegistry&) const;
    void advance_energy(double dt_days);
    void advance_moisture(double dt_days, double day);

    std::uint8_t reference_level_{};
    bool has_level_{};
    std::vector<ClimateNode> nodes_;
    std::map<CellId,std::size_t> index_;
    std::vector<Link> links_;
    std::vector<std::vector<WeatherSample>> weather_support_;
    ClimateBudget budget_;
    double ocean_water_m3_{};
    double atmospheric_carbon_kg_{};
    double ocean_carbon_kg_{};
};

// Coupled default-world inventory. Climate precipitation/evaporation ledgers
// are deliberately excluded because they are flux histories, not stocks.
double total_planet_water_m3(const WorldState&, const FieldRegistry&);
double total_planet_carbon_kg(const WorldState&, const FieldRegistry&);

} // namespace worldsim

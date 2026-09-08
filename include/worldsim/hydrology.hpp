#pragma once

#include "worldsim/simulation.hpp"

namespace worldsim {

// Engineering root-zone closure shared by hydrology and ecology.
double soil_water_capacity_depth_m(double regolith_thickness_m);

struct HydrologyNode {
    CellId cell;
    double bed_m{};
    double land_area_m2{};
    double surface_m3{};
    double discharge_m3_day{};
    double erosion_volume_m3{}; // integrated since the last geology pass
};

struct WaterBudget {
    double precipitation_m3{};
    double evaporation_m3{}; // includes transpiration
    double ocean_export_m3{};
};

// Persistent reference-grid reservoirs. Active-cover fields are projections of
// this graph; focus refinement never destroys a lake sill or changes its links.
class HydrologyStore final : public IStateStore {
public:
    static constexpr std::string_view kKey="hydrology.basins";
    std::string_view key() const override { return kKey; }
    std::uint32_t snapshot_version() const override { return 2; }
    void on_add_cell(CellId cell) override;
    void on_remove_cell(CellId) override {}
    void on_refine(CellId, std::span<const CellId>, const CubeSphereTopology&) override {}
    void on_coarsen(std::span<const CellId>, CellId, const CubeSphereTopology&) override {}
    void save(BinaryWriter&) const override;
    void load(BinaryReader&, std::uint32_t version) override;
    void validate_active_cover(const std::set<CellId>&) const override;

    void initialize(const WorldState&, const FieldRegistry&);
    const std::vector<HydrologyNode>& nodes() const { return nodes_; }
    const WaterBudget& budget() const { return budget_; }
    std::size_t node_index(CellId active_cell) const;
    double total_surface_m3() const;
    double depth_m(std::size_t node) const;
    double level_m(std::size_t node) const;
    double wetted_fraction(std::size_t node) const;
    double volume_at_level(std::size_t node, double level) const;
    double spill_level_m(std::size_t node) const { return spill_.at(node); }
    std::size_t basin_id(std::size_t node) const { return basin_.at(node)+1; }
    std::size_t lake_id(std::size_t node) const;
    double drainage_area_m2(std::size_t node) const { return drainage_area_.at(node); }

    // Explicit domain inputs, also useful for controlled headless experiments.
    void add_surface_water(CellId region, double volume_m3);
    void set_bed_elevation(CellId region, double elevation_m);
    void apply_bed_changes(const WorldState&, const std::map<CellId,double>& delta_m,
        const std::map<CellId,double>& land_fraction_delta={});
    void route(double dt_days); // simultaneous, donor-limited reference-link transfers
    void project(WorldState&, const FieldRegistry&) const;
    // Projects elapsed-window discharge into geology diagnostics, then consumes
    // the window. Terrain-only worlds keep their independent geology fallback.
    void consume_geology_discharge(WorldState&, const FieldRegistry&);
    // Climate consumes this transfer exactly once while the cumulative budget
    // remains available for land-hydrology diagnostics.
    double take_pending_ocean_export_m3();

private:
    friend class HydrologySystem;
    struct Link { std::size_t a{},b{}; double travel_days{}; };
    void rebuild_graph();
    void rebuild_drainage();
    void rebuild_lakes();
    double take_surface(std::size_t node, double requested_m3);
    void export_to_ocean(double volume_m3);
    std::uint8_t reference_level_{};
    bool has_level_{};
    std::vector<HydrologyNode> nodes_;
    std::map<CellId,std::size_t> index_;
    std::vector<Link> links_;
    std::vector<std::vector<std::size_t>> neighbors_;
    std::vector<double> spill_,drainage_area_;
    std::vector<std::size_t> basin_,lake_;
    WaterBudget budget_;
    double geology_days_{};
    double pending_ocean_export_m3_{};
};

class HydrologyModule final : public ISimModule {
public:
    std::string_view id() const override { return "hydrology"; }
    void register_fields(FieldRegistry&) override;
    void register_stores(StateStoreRegistry&, const FieldRegistry&) override;
    void register_systems(Scheduler&, const FieldRegistry&) override;
    void initialize(WorldState&, const FieldRegistry&) override;
    void on_spatial_cover_changed(WorldState&, const FieldRegistry&) override;
};

// Excludes projected surface diagnostics to avoid double counting the store.
double total_land_water_m3(const WorldState&, const FieldRegistry&);

} // namespace worldsim

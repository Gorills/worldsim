#pragma once

#include "world_simulation_node.hpp"

namespace worldsim::godot_adapter {

class SurvivalSimulationNode final : public WorldSimulationNode {
    GDCLASS(SurvivalSimulationNode,WorldSimulationNode)
public:
    void initialize_survival(std::int64_t seed);
    [[nodiscard]] godot::Dictionary get_local_resources(
        double east_m,
        double north_m
    ) const;
    [[nodiscard]] godot::Dictionary get_inventory() const;
    [[nodiscard]] bool has_stone_axe() const;
    [[nodiscard]] godot::Dictionary get_campsite(
        double east_m,
        double north_m
    ) const;
    bool collect_resource_at(
        double east_m,
        double north_m,
        const godot::String& resource_key,
        double amount
    );
    double gather_resource_at(
        double east_m,
        double north_m,
        const godot::String& resource_key
    );
    bool craft_stone_axe();
    bool build_basic_shelter_at(double east_m,double north_m);
    bool build_campfire_at(double east_m,double north_m);
    bool add_campfire_fuel_at(
        double east_m,
        double north_m,
        double wood_kg
    );
    bool set_campfire_lit_at(
        double east_m,
        double north_m,
        bool lit
    );

protected:
    static void _bind_methods();
};

} // namespace worldsim::godot_adapter
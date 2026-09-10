#include "survival_simulation_node.hpp"

#include "worldsim/resources.hpp"
#include "worldsim/terrain.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <exception>
#include <stdexcept>
#include <string>

namespace worldsim::godot_adapter {

using godot::ClassDB;
using godot::Dictionary;
using godot::String;

void SurvivalSimulationNode::_bind_methods() {
    ClassDB::bind_method(
        godot::D_METHOD("initialize_survival","seed"),
        &SurvivalSimulationNode::initialize_survival
    );
    ClassDB::bind_method(
        godot::D_METHOD("get_local_resources","east_m","north_m"),
        &SurvivalSimulationNode::get_local_resources
    );
    ClassDB::bind_method(
        godot::D_METHOD("get_inventory"),
        &SurvivalSimulationNode::get_inventory
    );
    ClassDB::bind_method(
        godot::D_METHOD("has_stone_axe"),
        &SurvivalSimulationNode::has_stone_axe
    );
    ClassDB::bind_method(
        godot::D_METHOD(
            "collect_resource_at",
            "east_m",
            "north_m",
            "resource_key",
            "amount"
        ),
        &SurvivalSimulationNode::collect_resource_at
    );
    ClassDB::bind_method(
        godot::D_METHOD(
            "gather_resource_at",
            "east_m",
            "north_m",
            "resource_key"
        ),
        &SurvivalSimulationNode::gather_resource_at
    );
    ClassDB::bind_method(
        godot::D_METHOD("craft_stone_axe"),
        &SurvivalSimulationNode::craft_stone_axe
    );
}

void SurvivalSimulationNode::initialize_survival(std::int64_t seed) {
    // Reuse the base initialization for terrain-preview/client state, then swap
    // only the authoritative simulation factory to the survival composition.
    WorldSimulationNode::initialize(seed);
    if (!last_error_.empty()) return;
    try {
        worldsim::SimulationConfig config;
        config.base_level=4;
        config.max_level=7;
        config.tick_seconds=3600.0;
        sim_=worldsim::make_survival_simulation(
            static_cast<std::uint64_t>(seed),
            config
        );
        last_error_.clear();
    } catch (const std::exception& error) {
        sim_.reset();
        report_error(error.what());
    } catch (...) {
        sim_.reset();
        report_error("unknown C++ exception during survival initialization");
    }
}

Dictionary SurvivalSimulationNode::get_local_resources(
    double east_m,
    double north_m
) const {
    Dictionary result;
    try {
        if (!sim_) throw std::runtime_error("survival simulation is not initialized");
        const auto direction=worldsim::TerrainGenerator::projected_to_direction(
            east_m,
            north_m
        );
        for (const auto& descriptor:worldsim::kResourceDescriptors) {
            const std::string key(descriptor.key);
            result[String(key.c_str())]=worldsim::resource_availability(
                *sim_,direction,descriptor.kind
            );
        }
        last_error_.clear();
    } catch (const std::exception& error) {
        report_error(error.what());
        result.clear();
    } catch (...) {
        report_error("unknown C++ exception in get_local_resources");
        result.clear();
    }
    return result;
}

Dictionary SurvivalSimulationNode::get_inventory() const {
    Dictionary result;
    try {
        if (!sim_) throw std::runtime_error("survival simulation is not initialized");
        for (const auto& descriptor:worldsim::kResourceDescriptors) {
            const std::string key(descriptor.key);
            result[String(key.c_str())]=worldsim::player_inventory_amount(
                *sim_,descriptor.kind
            );
        }
        last_error_.clear();
    } catch (const std::exception& error) {
        report_error(error.what());
        result.clear();
    } catch (...) {
        report_error("unknown C++ exception in get_inventory");
        result.clear();
    }
    return result;
}

bool SurvivalSimulationNode::has_stone_axe() const {
    try {
        if (!sim_) throw std::runtime_error("survival simulation is not initialized");
        const bool owned=worldsim::player_has_stone_axe(*sim_);
        last_error_.clear();
        return owned;
    } catch (const std::exception& error) {
        report_error(error.what());
        return false;
    } catch (...) {
        report_error("unknown C++ exception in has_stone_axe");
        return false;
    }
}

bool SurvivalSimulationNode::collect_resource_at(
    double east_m,
    double north_m,
    const String& resource_key,
    double amount
) {
    try {
        if (!sim_) throw std::runtime_error("survival simulation is not initialized");
        const std::string key=resource_key.utf8().get_data();
        const auto kind=worldsim::resource_kind_from_key(key);
        if (!kind) throw std::invalid_argument("unknown resource key");
        const auto direction=worldsim::TerrainGenerator::projected_to_direction(
            east_m,
            north_m
        );
        (void)worldsim::collect_resource(*sim_,direction,*kind,amount);
        last_error_.clear();
        return true;
    } catch (const std::exception& error) {
        report_error(error.what());
        return false;
    } catch (...) {
        report_error("unknown C++ exception in collect_resource_at");
        return false;
    }
}

double SurvivalSimulationNode::gather_resource_at(
    double east_m,
    double north_m,
    const String& resource_key
) {
    try {
        if (!sim_) throw std::runtime_error("survival simulation is not initialized");
        const std::string key=resource_key.utf8().get_data();
        const auto kind=worldsim::resource_kind_from_key(key);
        if (!kind) throw std::invalid_argument("unknown resource key");
        const auto direction=worldsim::TerrainGenerator::projected_to_direction(
            east_m,
            north_m
        );
        const double realized=worldsim::gather_resource(
            *sim_,direction,*kind
        );
        last_error_.clear();
        return realized;
    } catch (const std::exception& error) {
        // Unavailable resources are normal player feedback, not engine faults.
        last_error_=error.what();
        return 0.0;
    } catch (...) {
        last_error_="unknown C++ exception in gather_resource_at";
        return 0.0;
    }
}

bool SurvivalSimulationNode::craft_stone_axe() {
    try {
        if (!sim_) throw std::runtime_error("survival simulation is not initialized");
        worldsim::craft_stone_axe(*sim_);
        last_error_.clear();
        return true;
    } catch (const std::exception& error) {
        // Insufficient materials and duplicate craft attempts are expected
        // gameplay rejections surfaced through get_last_error().
        last_error_=error.what();
        return false;
    } catch (...) {
        last_error_="unknown C++ exception in craft_stone_axe";
        return false;
    }
}

} // namespace worldsim::godot_adapter

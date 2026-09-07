#pragma once

#include "worldsim/simulation.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace worldsim::godot_adapter {

class WorldSimulationNode : public godot::Node {
    GDCLASS(WorldSimulationNode,godot::Node)
public:
    WorldSimulationNode();
    ~WorldSimulationNode() override=default;

    void initialize(std::int64_t seed);
    void initialize_terrain_world(std::int64_t seed);
    void step_hours(std::int64_t hours);
    void set_focus_direction(const godot::Vector3& direction);
    void clear_focus();
    void set_focus_projected(double east_m, double north_m);
    [[nodiscard]] std::int64_t get_tick() const;
    [[nodiscard]] double sample_terrain_height(double east_m, double north_m) const;
    [[nodiscard]] godot::PackedFloat32Array sample_terrain_patch(double center_east_m,
                                                                  double center_north_m,
                                                                  double spacing_m,
                                                                  std::int64_t resolution) const;
    [[nodiscard]] godot::PackedFloat32Array sample_terrain_equirectangular(std::int64_t width,
                                                                            std::int64_t height) const;
    [[nodiscard]] godot::Dictionary sample_tectonics_equirectangular(std::int64_t width,
                                                                       std::int64_t height) const;

    // Active-cell arrays in every packet are aligned by index. Cell IDs are split into
    // unsigned 32-bit halves because GDScript integers are signed 64-bit values.
    [[nodiscard]] godot::Dictionary get_render_packet() const;
    [[nodiscard]] godot::Array get_field_descriptors() const;
    [[nodiscard]] godot::PackedFloat64Array get_field_values(const godot::String& field_key) const;
    [[nodiscard]] godot::Array drain_events();
    [[nodiscard]] godot::String get_last_error() const;

    void schedule_field_impulse(std::int64_t cell_id_hi,
                                std::int64_t cell_id_lo,
                                const godot::String& field_key,
                                double delta);

protected:
    static void _bind_methods();

private:
    void ensure_sim() const;
    void report_error(const char* message) const;
    std::unique_ptr<worldsim::Simulation> sim_;
    mutable std::string last_error_;
};

} // namespace worldsim::godot_adapter

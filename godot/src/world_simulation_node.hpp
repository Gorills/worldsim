#pragma once

#include "worldsim/simulation.hpp"
#include "worldsim/terrain.hpp"

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

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
    [[nodiscard]] godot::Vector3 projected_to_direction(double east_m, double north_m) const;
    [[nodiscard]] std::int64_t get_tick() const;
    [[nodiscard]] std::int64_t get_terrain_revision() const;
    [[nodiscard]] std::int64_t get_surface_revision() const;
    [[nodiscard]] double sample_terrain_height(double east_m, double north_m) const;
    [[nodiscard]] godot::PackedFloat32Array sample_terrain_patch(double center_east_m,
                                                                  double center_north_m,
                                                                  double spacing_m,
                                                                  std::int64_t resolution) const;
    [[nodiscard]] godot::Dictionary sample_surface_visual_patch(
        double center_east_m,
        double center_north_m,
        double spacing_m,
        std::int64_t resolution
    ) const;
    [[nodiscard]] godot::Dictionary sample_terrain_visual_patch(
        double center_east_m,
        double center_north_m,
        double spacing_m,
        std::int64_t resolution,
        double origin_east_m,
        double origin_north_m,
        double origin_height_m
    ) const;
    [[nodiscard]] godot::PackedFloat32Array sample_terrain_equirectangular(std::int64_t width,
                                                                            std::int64_t height) const;
    [[nodiscard]] godot::PackedFloat64Array sample_field_equirectangular(
        const godot::String& field_key,
        std::int64_t width,
        std::int64_t height,
        bool normalize_extensive) const;
    [[nodiscard]] godot::PackedInt32Array sample_lod_equirectangular(
        std::int64_t width,
        std::int64_t height) const;
    [[nodiscard]] godot::Dictionary inspect_direction(
        const godot::Vector3& direction) const;
    [[nodiscard]] godot::PackedFloat32Array sample_preview_terrain_equirectangular(
        std::int64_t width,
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
    [[nodiscard]] std::uint64_t terrain_surface_fingerprint() const;
    [[nodiscard]] std::uint64_t surface_visual_fingerprint() const;
    std::unique_ptr<worldsim::Simulation> sim_;
    std::unique_ptr<worldsim::TerrainGenerator> terrain_preview_;
    std::int64_t terrain_revision_{1};
    std::int64_t surface_revision_{1};
    mutable std::unordered_map<std::uint64_t,double> terrain_preview_anchor_cache_;
    mutable std::string last_error_;
};

} // namespace worldsim::godot_adapter

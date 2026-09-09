#include "world_simulation_node.hpp"
#include "worldsim/terrain.hpp"
#include "worldsim/tectonics.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace worldsim::godot_adapter {

using godot::Array;
using godot::ClassDB;
using godot::Dictionary;
using godot::PackedFloat32Array;
using godot::PackedFloat64Array;
using godot::PackedInt32Array;
using godot::PackedInt64Array;
using godot::PackedVector3Array;
using godot::String;
using godot::UtilityFunctions;
using godot::Vector3;

namespace {

constexpr int kTerrainReconstructionRings=5;
constexpr double kTerrainReconstructionSupportCells=3.0;

struct TerrainReconstructionSource {
    worldsim::Vec3d direction;
    double authoritative_elevation_m{};
    double preview_elevation_m{};
};

using TerrainStencil=std::vector<TerrainReconstructionSource>;
using TerrainStencilCache=std::unordered_map<std::uint64_t,TerrainStencil>;

std::vector<worldsim::CellId> reconstruction_cells(
    const worldsim::CubeSphereTopology& topology,
    worldsim::CellId center
) {
    std::map<worldsim::CellId,int> distances;
    std::deque<worldsim::CellId> pending;
    distances.emplace(center,0);
    pending.push_back(center);
    while (!pending.empty()) {
        const worldsim::CellId current=pending.front();
        pending.pop_front();
        const int distance=distances.at(current);
        if (distance>=kTerrainReconstructionRings) continue;
        for (worldsim::CellId neighbor:topology.neighbors4(current)) {
            if (!distances.emplace(neighbor,distance+1).second) continue;
            pending.push_back(neighbor);
        }
    }

    std::vector<worldsim::CellId> cells;
    cells.reserve(distances.size());
    for (const auto& [cell,distance]:distances) {
        (void)distance;
        cells.push_back(cell);
    }
    return cells;
}

TerrainStencil build_terrain_stencil(
    const worldsim::Simulation& sim,
    const worldsim::TerrainGenerator& terrain,
    worldsim::CellId center,
    std::unordered_map<std::uint64_t,double>& preview_anchor_cache
) {
    const auto& world=sim.world();
    const auto& fields=world.stores().get<worldsim::FieldStore>();
    const auto elevation=sim.fields().find("geography.elevation_m");
    if (!elevation)
        throw std::runtime_error("geography elevation field is missing");

    TerrainStencil stencil;
    const auto cells=reconstruction_cells(world.topology(),center);
    stencil.reserve(cells.size());
    for (worldsim::CellId cell:cells) {
        double authoritative=0.0;
        const auto parts=world.resolve_active_cover(cell);
        for (const worldsim::ActiveCoverPart& part:parts)
            authoritative+=fields.get(part.cell,*elevation)*part.weight;

        const worldsim::Vec3d direction=world.topology().center_unit(cell);
        const auto [it,inserted]=preview_anchor_cache.try_emplace(cell.raw(),0.0);
        if (inserted)
            it->second=terrain.sample_direction(direction).elevation_m;
        stencil.push_back({direction,authoritative,it->second});
    }
    return stencil;
}

double reconstructed_terrain_height(
    const worldsim::Simulation& sim,
    const worldsim::TerrainGenerator& terrain,
    worldsim::Vec3d direction,
    TerrainStencilCache& stencil_cache,
    std::unordered_map<std::uint64_t,double>& preview_anchor_cache
) {
    direction=worldsim::normalized(direction);
    const auto& topology=sim.world().topology();
    const std::uint8_t level=sim.config().max_level;
    const worldsim::CellId center=topology.from_direction(direction,level);
    auto [it,inserted]=stencil_cache.try_emplace(center.raw());
    if (inserted)
        it->second=build_terrain_stencil(sim,terrain,center,preview_anchor_cache);

    const double nominal_cell_angle=(0.5*worldsim::kPi)/static_cast<double>(1U<<level);
    const double support_angle=std::min(
        0.75*worldsim::kPi,
        kTerrainReconstructionSupportCells*nominal_cell_angle
    );
    const double support_chord=std::sqrt(2.0-2.0*std::cos(support_angle));
    double weight_sum=0.0;
    double authoritative_sum=0.0;
    double preview_sum=0.0;
    for (const TerrainReconstructionSource& source:it->second) {
        const double chord=std::sqrt(std::max(
            0.0,
            2.0-2.0*std::clamp(worldsim::dot(direction,source.direction),-1.0,1.0)
        ));
        const double radius=chord/support_chord;
        if (!(radius<1.0)) continue;
        const double remaining=1.0-radius;
        const double remaining2=remaining*remaining;
        const double weight=remaining2*remaining2*(1.0+4.0*radius);
        weight_sum+=weight;
        authoritative_sum+=weight*source.authoritative_elevation_m;
        preview_sum+=weight*source.preview_elevation_m;
    }
    if (!(weight_sum>0.0))
        throw std::runtime_error("terrain reconstruction stencil has no support");

    const double authoritative_macro=authoritative_sum/weight_sum;
    const double preview_macro=preview_sum/weight_sum;
    const double preview=terrain.sample_direction(direction).elevation_m;
    return std::clamp(
        authoritative_macro+(preview-preview_macro),
        -11'000.0,
        9'000.0
    );
}

struct LocalPlanetFrame {
    worldsim::Vec3d origin_surface;
    worldsim::Vec3d x_axis;
    worldsim::Vec3d up_axis;
    worldsim::Vec3d z_axis;
};

LocalPlanetFrame local_planet_frame(
    double origin_east_m,
    double origin_north_m,
    double origin_height_m
) {
    constexpr double derivative_step_m=16.0;
    const worldsim::Vec3d up=worldsim::TerrainGenerator::projected_to_direction(
        origin_east_m,
        origin_north_m
    );
    const worldsim::Vec3d east_direction=worldsim::TerrainGenerator::projected_to_direction(
        origin_east_m+derivative_step_m,
        origin_north_m
    );
    const worldsim::Vec3d north_direction=worldsim::TerrainGenerator::projected_to_direction(
        origin_east_m,
        origin_north_m+derivative_step_m
    );

    worldsim::Vec3d x=east_direction-up*worldsim::dot(east_direction,up);
    if (!(worldsim::norm(x)>1.0e-12))
        throw std::runtime_error("projected east axis is degenerate");
    x=worldsim::normalized(x);

    worldsim::Vec3d z=
        north_direction-
        up*worldsim::dot(north_direction,up)-
        x*worldsim::dot(north_direction,x);
    if (!(worldsim::norm(z)>1.0e-12))
        throw std::runtime_error("projected north axis is degenerate");
    z=worldsim::normalized(z);

    return {
        up*(worldsim::kEarthRadiusM+origin_height_m),
        x,
        up,
        z
    };
}

Vector3 local_planet_position(
    const LocalPlanetFrame& frame,
    worldsim::Vec3d direction,
    double elevation_m
) {
    const worldsim::Vec3d point=
        worldsim::normalized(direction)*(worldsim::kEarthRadiusM+elevation_m);
    const worldsim::Vec3d delta=point-frame.origin_surface;
    return {
        static_cast<godot::real_t>(worldsim::dot(delta,frame.x_axis)),
        static_cast<godot::real_t>(worldsim::dot(delta,frame.up_axis)),
        static_cast<godot::real_t>(worldsim::dot(delta,frame.z_axis))
    };
}

std::uint64_t fingerprint_mix(std::uint64_t hash, std::uint64_t value) {
    hash^=value+0x9e3779b97f4a7c15ULL+(hash<<6U)+(hash>>2U);
    return hash;
}

std::pair<int,int> checked_map_dimensions(
    std::int64_t width,
    std::int64_t height,
    std::string_view map_name
) {
    constexpr std::int64_t max_samples=2'097'152;
    if (width<2 || height<2 || width>max_samples/height)
        throw std::invalid_argument(
            std::string(map_name)+
            " map must be at least 2x2 and contain at most 2097152 samples"
        );
    return {static_cast<int>(width),static_cast<int>(height)};
}

worldsim::Vec3d equirectangular_pixel_direction(
    int x,
    int y,
    int width,
    int height
) {
    const double v=(static_cast<double>(y)+0.5)/static_cast<double>(height);
    const double latitude=(0.5-v)*worldsim::kPi;
    const double sin_lat=std::sin(latitude);
    const double cos_lat=std::cos(latitude);
    const double u=(static_cast<double>(x)+0.5)/static_cast<double>(width);
    const double longitude=(2.0*u-1.0)*worldsim::kPi;
    return {
        cos_lat*std::cos(longitude),
        cos_lat*std::sin(longitude),
        sin_lat
    };
}

worldsim::CellId active_cell_at_direction(
    const worldsim::Simulation& sim,
    worldsim::Vec3d direction
) {
    const auto& world=sim.world();
    const auto region=world.topology().from_direction(
        direction,
        sim.config().max_level
    );
    const auto parts=world.resolve_active_cover(region);
    if (parts.size()!=1U)
        throw std::runtime_error(
            "point query unexpectedly resolved to multiple active cells"
        );
    return parts.front().cell;
}

} // namespace

WorldSimulationNode::WorldSimulationNode() {
    initialize(1);
}

void WorldSimulationNode::_bind_methods() {
    ClassDB::bind_method(godot::D_METHOD("initialize","seed"),&WorldSimulationNode::initialize);
    ClassDB::bind_method(godot::D_METHOD("initialize_terrain_world","seed"),&WorldSimulationNode::initialize_terrain_world);
    ClassDB::bind_method(godot::D_METHOD("step_hours","hours"),&WorldSimulationNode::step_hours);
    ClassDB::bind_method(godot::D_METHOD("set_focus_direction","direction"),&WorldSimulationNode::set_focus_direction);
    ClassDB::bind_method(godot::D_METHOD("clear_focus"),&WorldSimulationNode::clear_focus);
    ClassDB::bind_method(godot::D_METHOD("set_focus_projected","east_m","north_m"),&WorldSimulationNode::set_focus_projected);
    ClassDB::bind_method(godot::D_METHOD("projected_to_direction","east_m","north_m"),
                         &WorldSimulationNode::projected_to_direction);
    ClassDB::bind_method(godot::D_METHOD("get_tick"),&WorldSimulationNode::get_tick);
    ClassDB::bind_method(godot::D_METHOD("get_terrain_revision"),&WorldSimulationNode::get_terrain_revision);
    ClassDB::bind_method(godot::D_METHOD("sample_terrain_height","east_m","north_m"),&WorldSimulationNode::sample_terrain_height);
    ClassDB::bind_method(godot::D_METHOD("sample_terrain_patch","center_east_m","center_north_m","spacing_m","resolution"),
                         &WorldSimulationNode::sample_terrain_patch);
    ClassDB::bind_method(
        godot::D_METHOD(
            "sample_terrain_visual_patch",
            "center_east_m",
            "center_north_m",
            "spacing_m",
            "resolution",
            "origin_east_m",
            "origin_north_m",
            "origin_height_m"
        ),
        &WorldSimulationNode::sample_terrain_visual_patch
    );
    ClassDB::bind_method(godot::D_METHOD("sample_terrain_equirectangular","width","height"),
                         &WorldSimulationNode::sample_terrain_equirectangular);
    ClassDB::bind_method(
        godot::D_METHOD(
            "sample_field_equirectangular",
            "field_key",
            "width",
            "height",
            "normalize_extensive"
        ),
        &WorldSimulationNode::sample_field_equirectangular
    );
    ClassDB::bind_method(
        godot::D_METHOD("sample_lod_equirectangular","width","height"),
        &WorldSimulationNode::sample_lod_equirectangular
    );
    ClassDB::bind_method(
        godot::D_METHOD("inspect_direction","direction"),
        &WorldSimulationNode::inspect_direction
    );
    ClassDB::bind_method(godot::D_METHOD("sample_preview_terrain_equirectangular","width","height"),
                         &WorldSimulationNode::sample_preview_terrain_equirectangular);
    ClassDB::bind_method(godot::D_METHOD("sample_tectonics_equirectangular","width","height"),
                         &WorldSimulationNode::sample_tectonics_equirectangular);
    ClassDB::bind_method(godot::D_METHOD("get_render_packet"),&WorldSimulationNode::get_render_packet);
    ClassDB::bind_method(godot::D_METHOD("get_field_descriptors"),&WorldSimulationNode::get_field_descriptors);
    ClassDB::bind_method(godot::D_METHOD("get_field_values","field_key"),&WorldSimulationNode::get_field_values);
    ClassDB::bind_method(godot::D_METHOD("drain_events"),&WorldSimulationNode::drain_events);
    ClassDB::bind_method(godot::D_METHOD("get_last_error"),&WorldSimulationNode::get_last_error);
    ClassDB::bind_method(godot::D_METHOD("schedule_field_impulse","cell_id_hi","cell_id_lo","field_key","delta"),
                         &WorldSimulationNode::schedule_field_impulse);
}

void WorldSimulationNode::report_error(const char* message) const {
    last_error_=message ? message : "unknown WorldSim error";
    UtilityFunctions::push_error(String(last_error_.c_str()));
}

void WorldSimulationNode::ensure_sim() const {
    if (!sim_) throw std::runtime_error("WorldSimulationNode is not initialized");
}

void WorldSimulationNode::initialize(std::int64_t seed) {
    try {
        worldsim::SimulationConfig cfg;
        cfg.base_level=4;
        cfg.max_level=7;
        cfg.tick_seconds=3600.0;
        sim_=worldsim::make_default_simulation(static_cast<std::uint64_t>(seed),cfg);
        terrain_preview_=std::make_unique<worldsim::TerrainGenerator>(
            static_cast<std::uint64_t>(seed)
        );
        terrain_revision_=1;
        terrain_preview_anchor_cache_.clear();
        last_error_.clear();
    } catch (const std::exception& e) {
        sim_.reset();
        terrain_preview_.reset();
        report_error(e.what());
    } catch (...) {
        sim_.reset();
        terrain_preview_.reset();
        report_error("unknown C++ exception during WorldSim initialization");
    }
}

void WorldSimulationNode::initialize_terrain_world(std::int64_t seed) {
    try {
        worldsim::SimulationConfig cfg;
        cfg.base_level=4;
        cfg.max_level=7;
        cfg.tick_seconds=3600.0;
        sim_=worldsim::make_terrain_simulation(static_cast<std::uint64_t>(seed),cfg);
        terrain_preview_=std::make_unique<worldsim::TerrainGenerator>(
            static_cast<std::uint64_t>(seed)
        );
        terrain_revision_=1;
        terrain_preview_anchor_cache_.clear();
        last_error_.clear();
    } catch (const std::exception& e) {
        sim_.reset();
        terrain_preview_.reset();
        report_error(e.what());
    } catch (...) {
        sim_.reset();
        terrain_preview_.reset();
        report_error("unknown C++ exception during terrain world initialization");
    }
}

void WorldSimulationNode::step_hours(std::int64_t hours) {
    try {
        ensure_sim();
        if (hours<0) throw std::invalid_argument("hours must be non-negative");
        const std::uint64_t before=terrain_surface_fingerprint();
        sim_->step(static_cast<worldsim::Tick>(hours));
        const std::uint64_t after=terrain_surface_fingerprint();
        if (before!=after && terrain_revision_<std::numeric_limits<std::int64_t>::max())
            ++terrain_revision_;
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in step_hours"); }
}

void WorldSimulationNode::set_focus_direction(const Vector3& d) {
    try {
        ensure_sim();
        sim_->set_focus({static_cast<double>(d.x),static_cast<double>(d.y),static_cast<double>(d.z)});
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in set_focus_direction"); }
}

void WorldSimulationNode::set_focus_projected(double east_m, double north_m) {
    try {
        ensure_sim();
        if (!std::isfinite(east_m) || !std::isfinite(north_m))
            throw std::invalid_argument("projected focus must be finite");
        sim_->set_focus(worldsim::TerrainGenerator::projected_to_direction(east_m,north_m));
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in set_focus_projected"); }
}

Vector3 WorldSimulationNode::projected_to_direction(double east_m, double north_m) const {
    try {
        ensure_sim();
        if (!std::isfinite(east_m) || !std::isfinite(north_m))
            throw std::invalid_argument("projected coordinates must be finite");
        const auto direction=worldsim::TerrainGenerator::projected_to_direction(east_m,north_m);
        last_error_.clear();
        return {
            static_cast<godot::real_t>(direction.x),
            static_cast<godot::real_t>(direction.y),
            static_cast<godot::real_t>(direction.z)
        };
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) { report_error("unknown C++ exception in projected_to_direction"); return {}; }
}

void WorldSimulationNode::clear_focus() {
    try {
        ensure_sim();
        sim_->clear_focus();
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in clear_focus"); }
}

std::int64_t WorldSimulationNode::get_tick() const {
    try {
        ensure_sim();
        last_error_.clear();
        return static_cast<std::int64_t>(sim_->world().tick());
    } catch (const std::exception& e) { report_error(e.what()); return 0; }
    catch (...) { report_error("unknown C++ exception in get_tick"); return 0; }
}

std::int64_t WorldSimulationNode::get_terrain_revision() const {
    try {
        ensure_sim();
        last_error_.clear();
        return terrain_revision_;
    } catch (const std::exception& e) { report_error(e.what()); return 0; }
    catch (...) { report_error("unknown C++ exception in get_terrain_revision"); return 0; }
}

std::uint64_t WorldSimulationNode::terrain_surface_fingerprint() const {
    ensure_sim();
    const auto elevation=sim_->fields().find("geography.elevation_m");
    if (!elevation) throw std::runtime_error("geography elevation field is missing");
    const auto& fields=sim_->world().stores().get<worldsim::FieldStore>();
    std::uint64_t hash=0x243f6a8885a308d3ULL;
    for (worldsim::CellId cell:sim_->world().active_cells()) {
        hash=fingerprint_mix(hash,cell.raw());
        hash=fingerprint_mix(hash,std::bit_cast<std::uint64_t>(fields.get(cell,*elevation)));
    }
    return hash;
}

double WorldSimulationNode::sample_terrain_height(double east_m, double north_m) const {
    try {
        ensure_sim();
        if (!std::isfinite(east_m) || !std::isfinite(north_m))
            throw std::invalid_argument("terrain coordinates must be finite");
        if (!terrain_preview_) throw std::runtime_error("terrain preview is not initialized");
        TerrainStencilCache stencil_cache;
        const double height=reconstructed_terrain_height(
            *sim_,
            *terrain_preview_,
            worldsim::TerrainGenerator::projected_to_direction(east_m,north_m),
            stencil_cache,
            terrain_preview_anchor_cache_
        );
        last_error_.clear();
        return height;
    } catch (const std::exception& e) { report_error(e.what()); return 0.0; }
    catch (...) { report_error("unknown C++ exception in sample_terrain_height"); return 0.0; }
}

PackedFloat32Array WorldSimulationNode::sample_terrain_patch(double center_east_m,
                                                             double center_north_m,
                                                             double spacing_m,
                                                             std::int64_t resolution) const {
    PackedFloat32Array out;
    try {
        ensure_sim();
        if (!std::isfinite(center_east_m) || !std::isfinite(center_north_m) ||
            !std::isfinite(spacing_m) || spacing_m<=0.0)
            throw std::invalid_argument("invalid terrain patch coordinates or spacing");
        if (resolution<2 || resolution>129)
            throw std::invalid_argument("terrain patch resolution must be in [2,129]");

        const auto n=static_cast<int>(resolution);
        out.resize(n*n);
        const double half=0.5*static_cast<double>(n-1);
        if (!terrain_preview_) throw std::runtime_error("terrain preview is not initialized");
        TerrainStencilCache stencil_cache;
        for (int z=0;z<n;++z) {
            for (int x=0;x<n;++x) {
                const double east=center_east_m+(static_cast<double>(x)-half)*spacing_m;
                const double north=center_north_m+(static_cast<double>(z)-half)*spacing_m;
                const double height=reconstructed_terrain_height(
                    *sim_,
                    *terrain_preview_,
                    worldsim::TerrainGenerator::projected_to_direction(east,north),
                    stencil_cache,
                    terrain_preview_anchor_cache_
                );
                out.set(z*n+x,static_cast<float>(height));
            }
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in sample_terrain_patch"); }
    return out;
}

Dictionary WorldSimulationNode::sample_terrain_visual_patch(
    double center_east_m,
    double center_north_m,
    double spacing_m,
    std::int64_t resolution,
    double origin_east_m,
    double origin_north_m,
    double origin_height_m
) const {
    Dictionary out;
    try {
        ensure_sim();
        if (!std::isfinite(center_east_m) || !std::isfinite(center_north_m) ||
            !std::isfinite(spacing_m) || spacing_m<=0.0 ||
            !std::isfinite(origin_east_m) || !std::isfinite(origin_north_m) ||
            !std::isfinite(origin_height_m))
            throw std::invalid_argument("invalid visual terrain patch coordinates or spacing");
        if (resolution<2 || resolution>129)
            throw std::invalid_argument("visual terrain patch resolution must be in [2,129]");
        if (!terrain_preview_)
            throw std::runtime_error("terrain preview is not initialized");

        const auto n=static_cast<int>(resolution);
        const int sample_count=n*n;
        const double half=0.5*static_cast<double>(n-1);
        PackedVector3Array positions;
        PackedVector3Array sea_positions;
        PackedFloat32Array heights;
        positions.resize(sample_count);
        sea_positions.resize(sample_count);
        heights.resize(sample_count);

        const LocalPlanetFrame frame=local_planet_frame(
            origin_east_m,
            origin_north_m,
            origin_height_m
        );
        TerrainStencilCache stencil_cache;
        for (int z=0;z<n;++z) {
            for (int x=0;x<n;++x) {
                const int index=z*n+x;
                const double east=
                    center_east_m+(static_cast<double>(x)-half)*spacing_m;
                const double north=
                    center_north_m+(static_cast<double>(z)-half)*spacing_m;
                const worldsim::Vec3d direction=
                    worldsim::TerrainGenerator::projected_to_direction(east,north);
                const double height=reconstructed_terrain_height(
                    *sim_,
                    *terrain_preview_,
                    direction,
                    stencil_cache,
                    terrain_preview_anchor_cache_
                );
                positions.set(index,local_planet_position(frame,direction,height));
                sea_positions.set(index,local_planet_position(frame,direction,0.0));
                heights.set(index,static_cast<float>(height));
            }
        }

        out["positions"]=positions;
        out["sea_positions"]=sea_positions;
        out["heights"]=heights;
        last_error_.clear();
    } catch (const std::exception& e) {
        report_error(e.what());
    } catch (...) {
        report_error("unknown C++ exception in sample_terrain_visual_patch");
    }
    return out;
}

PackedFloat32Array WorldSimulationNode::sample_terrain_equirectangular(std::int64_t width,
                                                                             std::int64_t height) const {
    PackedFloat32Array out;
    try {
        ensure_sim();
        const auto [w,h]=checked_map_dimensions(width,height,"global terrain");
        out.resize(w*h);
        const auto& world=sim_->world();
        const auto& fields=world.stores().get<worldsim::FieldStore>();
        const auto elevation=sim_->fields().find("geography.elevation_m");
        if (!elevation) throw std::runtime_error("geography elevation field is missing");

        // Sample pixel centers so the equirectangular texture contains neither a
        // duplicated +/-180 degree column nor exact pole singularities.
        for (int y=0;y<h;++y) {
            for (int x=0;x<w;++x) {
                // A diagnostic map displays the active simulation cover exactly.
                // Query a maximum-simulation-level region and resolve its active ancestor;
                // regenerating seed terrain here hides all geological evolution.
                const auto cell=active_cell_at_direction(
                    *sim_,
                    equirectangular_pixel_direction(x,y,w,h)
                );
                out.set(y*w+x,static_cast<float>(fields.get(cell,*elevation)));
            }
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) { report_error("unknown C++ exception in sample_terrain_equirectangular"); return {}; }
    return out;
}

PackedFloat64Array WorldSimulationNode::sample_field_equirectangular(
    const String& field_key,
    std::int64_t width,
    std::int64_t height,
    bool normalize_extensive
) const {
    PackedFloat64Array out;
    try {
        ensure_sim();
        const auto [w,h]=checked_map_dimensions(width,height,"field diagnostic");
        const std::string key(field_key.utf8().get_data());
        const auto field=sim_->fields().find(key);
        if (!field) throw std::invalid_argument("unknown field key: "+key);

        const auto& descriptor=sim_->fields().descriptor(*field);
        const auto& world=sim_->world();
        const auto& fields=world.stores().get<worldsim::FieldStore>();
        const bool density=
            normalize_extensive &&
            descriptor.semantics==worldsim::FieldSemantics::Extensive;

        out.resize(w*h);
        for (int y=0;y<h;++y) {
            for (int x=0;x<w;++x) {
                const auto cell=active_cell_at_direction(
                    *sim_,
                    equirectangular_pixel_direction(x,y,w,h)
                );
                double value=fields.get(cell,*field);
                if (density) value/=world.topology().area_m2(cell);
                out.set(y*w+x,value);
            }
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) {
        report_error("unknown C++ exception in sample_field_equirectangular");
        return {};
    }
    return out;
}

PackedInt32Array WorldSimulationNode::sample_lod_equirectangular(
    std::int64_t width,
    std::int64_t height
) const {
    PackedInt32Array out;
    try {
        ensure_sim();
        const auto [w,h]=checked_map_dimensions(width,height,"LOD diagnostic");
        out.resize(w*h);
        for (int y=0;y<h;++y) {
            for (int x=0;x<w;++x) {
                const auto cell=active_cell_at_direction(
                    *sim_,
                    equirectangular_pixel_direction(x,y,w,h)
                );
                out.set(y*w+x,static_cast<std::int32_t>(cell.level()));
            }
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) {
        report_error("unknown C++ exception in sample_lod_equirectangular");
        return {};
    }
    return out;
}

Dictionary WorldSimulationNode::inspect_direction(const Vector3& direction) const {
    Dictionary out;
    try {
        ensure_sim();
        const worldsim::Vec3d query{
            static_cast<double>(direction.x),
            static_cast<double>(direction.y),
            static_cast<double>(direction.z)
        };
        const auto cell=active_cell_at_direction(*sim_,query);
        const auto& world=sim_->world();
        const auto& fields=world.stores().get<worldsim::FieldStore>();
        const std::uint64_t raw=cell.raw();

        Dictionary values;
        for (
            worldsim::FieldId id=0;
            id<static_cast<worldsim::FieldId>(sim_->fields().size());
            ++id
        ) {
            const auto& descriptor=sim_->fields().descriptor(id);
            values[String(descriptor.key.c_str())]=fields.get(cell,id);
        }

        out["cell_id_hi"]=static_cast<std::int64_t>(raw>>32U);
        out["cell_id_lo"]=static_cast<std::int64_t>(raw&0xffffffffULL);
        out["level"]=static_cast<std::int64_t>(cell.level());
        out["area_m2"]=world.topology().area_m2(cell);
        out["values"]=values;
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) {
        report_error("unknown C++ exception in inspect_direction");
        return {};
    }
    return out;
}

PackedFloat32Array WorldSimulationNode::sample_preview_terrain_equirectangular(
    std::int64_t width,
    std::int64_t height) const {
    PackedFloat32Array out;
    try {
        ensure_sim();
        constexpr std::int64_t max_samples=2'097'152;
        if (width<2 || height<2 || width>max_samples/height)
            throw std::invalid_argument("preview terrain map must be at least 2x2 and contain at most 2097152 samples");

        const int w=static_cast<int>(width);
        const int h=static_cast<int>(height);
        out.resize(w*h);
        const worldsim::TerrainGenerator terrain(sim_->world().seed());
        for (int y=0;y<h;++y) {
            const double v=(static_cast<double>(y)+0.5)/static_cast<double>(h);
            const double latitude=(0.5-v)*worldsim::kPi;
            const double sin_lat=std::sin(latitude);
            const double cos_lat=std::cos(latitude);
            for (int x=0;x<w;++x) {
                const double u=(static_cast<double>(x)+0.5)/static_cast<double>(w);
                const double longitude=(2.0*u-1.0)*worldsim::kPi;
                const worldsim::Vec3d direction{
                    cos_lat*std::cos(longitude),
                    cos_lat*std::sin(longitude),
                    sin_lat
                };
                out.set(y*w+x,static_cast<float>(terrain.sample_direction(direction).elevation_m));
            }
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) { report_error("unknown C++ exception in sample_preview_terrain_equirectangular"); return {}; }
    return out;
}

Dictionary WorldSimulationNode::sample_tectonics_equirectangular(std::int64_t width,
                                                                  std::int64_t height) const {
    try {
        ensure_sim();
        constexpr std::int64_t max_samples=2'097'152;
        if (width<2 || height<2 || width>max_samples/height)
            throw std::invalid_argument("global tectonic map must be at least 2x2 and contain at most 2097152 samples");

        const int w=static_cast<int>(width);
        const int h=static_cast<int>(height);
        PackedInt32Array plate_ids;
        PackedFloat32Array forcing;
        PackedFloat32Array uplift_forcing;
        PackedFloat32Array divergence_forcing;
        PackedFloat32Array crust_affinity;
        PackedFloat32Array macro_elevation;
        plate_ids.resize(w*h);
        forcing.resize(w*h);
        uplift_forcing.resize(w*h);
        divergence_forcing.resize(w*h);
        crust_affinity.resize(w*h);
        macro_elevation.resize(w*h);
        const worldsim::TectonicModel tectonics(sim_->world().seed());

        for (int y=0;y<h;++y) {
            const double v=(static_cast<double>(y)+0.5)/static_cast<double>(h);
            const double latitude=(0.5-v)*worldsim::kPi;
            const double sin_lat=std::sin(latitude);
            const double cos_lat=std::cos(latitude);
            for (int x=0;x<w;++x) {
                const double u=(static_cast<double>(x)+0.5)/static_cast<double>(w);
                const double longitude=(2.0*u-1.0)*worldsim::kPi;
                const worldsim::Vec3d direction{
                    cos_lat*std::cos(longitude),
                    cos_lat*std::sin(longitude),
                    sin_lat
                };
                const worldsim::TectonicSample sample=tectonics.sample_direction(direction);
                const int index=y*w+x;
                plate_ids.set(index,static_cast<std::int32_t>(sample.plate_id));
                forcing.set(index,static_cast<float>(sample.boundary_forcing));
                uplift_forcing.set(index,static_cast<float>(sample.uplift_forcing));
                divergence_forcing.set(index,static_cast<float>(sample.divergence_forcing));
                crust_affinity.set(index,static_cast<float>(sample.continental_affinity));
                macro_elevation.set(index,static_cast<float>(sample.macro_elevation_m));
            }
        }

        Dictionary out;
        out["plate_count"]=static_cast<std::int64_t>(worldsim::TectonicModel::kPlateCount);
        out["plate_id"]=plate_ids;
        out["forcing"]=forcing;
        out["uplift_forcing"]=uplift_forcing;
        out["divergence_forcing"]=divergence_forcing;
        out["crust_affinity"]=crust_affinity;
        out["macro_elevation_m"]=macro_elevation;
        last_error_.clear();
        return out;
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) { report_error("unknown C++ exception in sample_tectonics_equirectangular"); return {}; }
}

Dictionary WorldSimulationNode::get_render_packet() const {
    try {
        ensure_sim();
        const auto& world=sim_->world();
        const auto& fs=world.stores().get<worldsim::FieldStore>();
        const auto temp=sim_->fields().find("climate.surface_temperature_k");
        const auto veg=sim_->fields().find("ecology.vegetation_carbon_kg");
        const auto mana=sim_->fields().find("magic.mana_j");
        if (!temp || !veg || !mana)
            throw std::runtime_error("render packet requires the default simulation field set");
        const auto& cells=fs.dense_cells();
        const int n=static_cast<int>(cells.size());

        PackedVector3Array positions; positions.resize(n);
        PackedFloat64Array areas; areas.resize(n);
        PackedFloat32Array temperatures; temperatures.resize(n);
        PackedFloat32Array vegetation_density; vegetation_density.resize(n);
        PackedFloat32Array mana_density; mana_density.resize(n);
        PackedInt32Array levels; levels.resize(n);
        PackedInt64Array id_hi; id_hi.resize(n);
        PackedInt64Array id_lo; id_lo.resize(n);

        for (int i=0;i<n;++i) {
            const auto dense_index=static_cast<std::size_t>(i);
            const worldsim::CellId c=cells[dense_index];
            const auto p=world.topology().center_unit(c);
            const double area=world.topology().area_m2(c);
            const std::uint64_t raw=c.raw();
            positions.set(i,Vector3(static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z)));
            areas.set(i,area);
            temperatures.set(i,static_cast<float>(fs.get_dense(dense_index,*temp)-273.15));
            vegetation_density.set(i,static_cast<float>(fs.get_dense(dense_index,*veg)/area));
            mana_density.set(i,static_cast<float>(fs.get_dense(dense_index,*mana)/area));
            levels.set(i,static_cast<std::int32_t>(c.level()));
            id_hi.set(i,static_cast<std::int64_t>(raw>>32U));
            id_lo.set(i,static_cast<std::int64_t>(raw&0xffffffffULL));
        }

        Dictionary out;
        out["positions"]=positions;
        out["areas_m2"]=areas;
        out["temperature_c"]=temperatures;
        out["vegetation_kg_m2"]=vegetation_density;
        out["mana_j_m2"]=mana_density;
        out["levels"]=levels;
        out["cell_id_hi"]=id_hi;
        out["cell_id_lo"]=id_lo;
        last_error_.clear();
        return out;
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) { report_error("unknown C++ exception in get_render_packet"); return {}; }
}

Array WorldSimulationNode::get_field_descriptors() const {
    Array out;
    try {
        ensure_sim();
        for (worldsim::FieldId id=0;id<static_cast<worldsim::FieldId>(sim_->fields().size());++id) {
            const auto& d=sim_->fields().descriptor(id);
            Dictionary entry;
            entry["id"]=static_cast<std::int64_t>(id);
            entry["key"]=String(d.key.c_str());
            entry["unit"]=String(d.unit.c_str());
            entry["semantics"]=d.semantics==worldsim::FieldSemantics::Extensive ? String("extensive") : String("intensive");
            entry["default_value"]=d.default_value;
            entry["min_value"]=d.min_value;
            entry["max_value"]=d.max_value;
            out.push_back(entry);
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in get_field_descriptors"); }
    return out;
}

PackedFloat64Array WorldSimulationNode::get_field_values(const String& field_key) const {
    PackedFloat64Array out;
    try {
        ensure_sim();
        const std::string key(field_key.utf8().get_data());
        const auto field=sim_->fields().find(key);
        if (!field) throw std::invalid_argument("unknown field key: "+key);
        const auto& world=sim_->world();
        const auto& fs=world.stores().get<worldsim::FieldStore>();
        const auto& cells=fs.dense_cells();
        out.resize(static_cast<int>(cells.size()));
        for (std::size_t i=0;i<cells.size();++i)
            out.set(static_cast<int>(i),fs.get_dense(i,*field));
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in get_field_values"); }
    return out;
}

Array WorldSimulationNode::drain_events() {
    Array out;
    try {
        ensure_sim();
        for (const auto& e:sim_->world().drain_events()) {
            const std::uint64_t raw=e.cell.raw();
            Dictionary event;
            event["tick"]=static_cast<std::int64_t>(e.tick);
            event["type"]=String(e.type.c_str());
            event["cell_id_hi"]=static_cast<std::int64_t>(raw>>32U);
            event["cell_id_lo"]=static_cast<std::int64_t>(raw&0xffffffffULL);
            event["subject"]=static_cast<std::int64_t>(e.subject);
            event["magnitude"]=e.magnitude;
            out.push_back(event);
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in drain_events"); }
    return out;
}

String WorldSimulationNode::get_last_error() const {
    return String(last_error_.c_str());
}

void WorldSimulationNode::schedule_field_impulse(std::int64_t cell_id_hi,
                                                 std::int64_t cell_id_lo,
                                                 const String& field_key,
                                                 double delta) {
    try {
        ensure_sim();
        if (cell_id_hi<0 || cell_id_hi>0xffffffffLL || cell_id_lo<0 || cell_id_lo>0xffffffffLL)
            throw std::invalid_argument("cell id halves must be unsigned 32-bit values");
        const auto raw=(static_cast<std::uint64_t>(cell_id_hi)<<32U)|static_cast<std::uint64_t>(cell_id_lo);
        const std::string key(field_key.utf8().get_data());
        sim_->schedule_field_impulse(sim_->world().tick(),worldsim::CellId(raw),key,delta);
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in schedule_field_impulse"); }
}

} // namespace worldsim::godot_adapter

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
#include <cmath>
#include <exception>
#include <stdexcept>
#include <string>

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
    ClassDB::bind_method(godot::D_METHOD("sample_terrain_height","east_m","north_m"),&WorldSimulationNode::sample_terrain_height);
    ClassDB::bind_method(godot::D_METHOD("sample_terrain_patch","center_east_m","center_north_m","spacing_m","resolution"),
                         &WorldSimulationNode::sample_terrain_patch);
    ClassDB::bind_method(godot::D_METHOD("sample_terrain_equirectangular","width","height"),
                         &WorldSimulationNode::sample_terrain_equirectangular);
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
        last_error_.clear();
    } catch (const std::exception& e) {
        sim_.reset();
        report_error(e.what());
    } catch (...) {
        sim_.reset();
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
        last_error_.clear();
    } catch (const std::exception& e) {
        sim_.reset();
        report_error(e.what());
    } catch (...) {
        sim_.reset();
        report_error("unknown C++ exception during terrain world initialization");
    }
}

void WorldSimulationNode::step_hours(std::int64_t hours) {
    try {
        ensure_sim();
        if (hours<0) throw std::invalid_argument("hours must be non-negative");
        sim_->step(static_cast<worldsim::Tick>(hours));
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

double WorldSimulationNode::sample_terrain_height(double east_m, double north_m) const {
    try {
        ensure_sim();
        if (!std::isfinite(east_m) || !std::isfinite(north_m))
            throw std::invalid_argument("terrain coordinates must be finite");
        const worldsim::TerrainGenerator terrain(sim_->world().seed());
        last_error_.clear();
        return terrain.sample_projected(east_m,north_m).elevation_m;
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
        const worldsim::TerrainGenerator terrain(sim_->world().seed());
        for (int z=0;z<n;++z) {
            for (int x=0;x<n;++x) {
                const double east=center_east_m+(static_cast<double>(x)-half)*spacing_m;
                const double north=center_north_m+(static_cast<double>(z)-half)*spacing_m;
                const double height=terrain.sample_projected(east,north).elevation_m;
                out.set(z*n+x,static_cast<float>(height));
            }
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); }
    catch (...) { report_error("unknown C++ exception in sample_terrain_patch"); }
    return out;
}

PackedFloat32Array WorldSimulationNode::sample_terrain_equirectangular(std::int64_t width,
                                                                             std::int64_t height) const {
    PackedFloat32Array out;
    try {
        ensure_sim();
        constexpr std::int64_t max_samples=2'097'152;
        if (width<2 || height<2 || width>max_samples/height)
            throw std::invalid_argument("global terrain map must be at least 2x2 and contain at most 2097152 samples");

        const int w=static_cast<int>(width);
        const int h=static_cast<int>(height);
        out.resize(w*h);
        const auto& world=sim_->world();
        const auto& fields=world.stores().get<worldsim::FieldStore>();
        const auto elevation=sim_->fields().find("geography.elevation_m");
        if (!elevation) throw std::runtime_error("geography elevation field is missing");

        // Sample pixel centers so the equirectangular texture contains neither a
        // duplicated +/-180 degree column nor exact pole singularities.
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
                // A diagnostic map displays the active simulation cover exactly.
                // Query a finest-level region and resolve its active ancestor;
                // regenerating seed terrain here hides all geological evolution.
                const auto region=world.topology().from_direction(direction,worldsim::CellId::kMaxLevel);
                const auto parts=world.resolve_active_cover(region);
                out.set(y*w+x,static_cast<float>(fields.get(parts.front().cell,*elevation)));
            }
        }
        last_error_.clear();
    } catch (const std::exception& e) { report_error(e.what()); return {}; }
    catch (...) { report_error("unknown C++ exception in sample_terrain_equirectangular"); return {}; }
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

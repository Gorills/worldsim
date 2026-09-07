#include "world_simulation_node.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
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
    ClassDB::bind_method(godot::D_METHOD("step_hours","hours"),&WorldSimulationNode::step_hours);
    ClassDB::bind_method(godot::D_METHOD("set_focus_direction","direction"),&WorldSimulationNode::set_focus_direction);
    ClassDB::bind_method(godot::D_METHOD("clear_focus"),&WorldSimulationNode::clear_focus);
    ClassDB::bind_method(godot::D_METHOD("get_tick"),&WorldSimulationNode::get_tick);
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

Dictionary WorldSimulationNode::get_render_packet() const {
    try {
        ensure_sim();
        const auto& world=sim_->world();
        const auto& fs=world.stores().get<worldsim::FieldStore>();
        const auto temp=*sim_->fields().find("climate.surface_temperature_k");
        const auto veg=*sim_->fields().find("ecology.vegetation_carbon_kg");
        const auto mana=*sim_->fields().find("magic.mana_j");
        const int n=static_cast<int>(world.active_cells().size());

        PackedVector3Array positions; positions.resize(n);
        PackedFloat64Array areas; areas.resize(n);
        PackedFloat32Array temperatures; temperatures.resize(n);
        PackedFloat32Array vegetation_density; vegetation_density.resize(n);
        PackedFloat32Array mana_density; mana_density.resize(n);
        PackedInt32Array levels; levels.resize(n);
        PackedInt64Array id_hi; id_hi.resize(n);
        PackedInt64Array id_lo; id_lo.resize(n);

        int i=0;
        for (worldsim::CellId c:world.active_cells()) {
            const auto p=world.topology().center_unit(c);
            const double area=world.topology().area_m2(c);
            const std::uint64_t raw=c.raw();
            positions.set(i,Vector3(static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z)));
            areas.set(i,area);
            temperatures.set(i,static_cast<float>(fs.get(c,temp)-273.15));
            vegetation_density.set(i,static_cast<float>(fs.get(c,veg)/area));
            mana_density.set(i,static_cast<float>(fs.get(c,mana)/area));
            levels.set(i,static_cast<std::int32_t>(c.level()));
            id_hi.set(i,static_cast<std::int64_t>(raw>>32U));
            id_lo.set(i,static_cast<std::int64_t>(raw&0xffffffffULL));
            ++i;
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
        out.resize(static_cast<int>(world.active_cells().size()));
        int i=0;
        for (worldsim::CellId c:world.active_cells()) out.set(i++,fs.get(c,*field));
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

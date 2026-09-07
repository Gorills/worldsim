#include "worldsim/c_api.h"
#include "worldsim/simulation.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace worldsim;

struct ws_handle {
    std::unique_ptr<Simulation> sim;
    mutable std::string last_error;
};

namespace {
template<class F>
int guard(ws_handle* h,F&& f) {
    if (!h) return 0;
    try { f(); h->last_error.clear(); return 1; }
    catch (const std::exception& e) { h->last_error=e.what(); return 0; }
    catch (...) { h->last_error="unknown C++ exception"; return 0; }
}

size_t copy_text(std::string_view text,char* out,size_t capacity) {
    const size_t required=text.size()+1U;
    if (out && capacity>0U) {
        const size_t count=std::min(text.size(),capacity-1U);
        if (count>0U) std::memcpy(out,text.data(),count);
        out[count]='\0';
    }
    return required;
}
}

extern "C" {

uint32_t ws_abi_version(void) { return 1; }

ws_handle* ws_create_default(uint64_t seed) {
    try {
        auto* h=new ws_handle;
        h->sim=make_default_simulation(seed);
        return h;
    } catch (...) { return nullptr; }
}

void ws_destroy(ws_handle* handle) { delete handle; }

int ws_step(ws_handle* handle,uint64_t ticks) { return guard(handle,[&]{ handle->sim->step(ticks); }); }
uint64_t ws_tick(const ws_handle* handle) { return handle ? handle->sim->world().tick() : 0; }

int ws_set_focus(ws_handle* handle,double x,double y,double z) { return guard(handle,[&]{ handle->sim->set_focus({x,y,z}); }); }
int ws_clear_focus(ws_handle* handle) { return guard(handle,[&]{ handle->sim->clear_focus(); }); }

size_t ws_cell_count(const ws_handle* handle) { return handle ? handle->sim->world().active_cells().size() : 0; }

size_t ws_copy_cells(const ws_handle* handle,ws_cell_v1* out_cells,size_t capacity) {
    if (!handle || !out_cells) return 0;
    size_t i=0;
    for (CellId c:handle->sim->world().active_cells()) {
        if (i>=capacity) break;
        const auto p=handle->sim->world().topology().center_unit(c);
        out_cells[i]={c.raw(),c.level(),{0,0,0,0,0,0,0},p.x,p.y,p.z,handle->sim->world().topology().area_m2(c)};
        ++i;
    }
    return i;
}

size_t ws_field_count(const ws_handle* handle) {
    return handle ? handle->sim->fields().size() : 0U;
}

int ws_find_field(const ws_handle* handle,const char* key,uint32_t* out_field_id) {
    if (!handle || !key || !out_field_id) return 0;
    const auto id=handle->sim->fields().find(key);
    if (!id) return 0;
    *out_field_id=*id;
    return 1;
}

size_t ws_field_key(const ws_handle* handle,uint32_t field_id,char* out_text,size_t capacity) {
    if (!handle) return 0U;
    try { return copy_text(handle->sim->fields().descriptor(field_id).key,out_text,capacity); }
    catch (const std::exception& e) { handle->last_error=e.what(); return 0U; }
}

size_t ws_field_unit(const ws_handle* handle,uint32_t field_id,char* out_text,size_t capacity) {
    if (!handle) return 0U;
    try { return copy_text(handle->sim->fields().descriptor(field_id).unit,out_text,capacity); }
    catch (const std::exception& e) { handle->last_error=e.what(); return 0U; }
}

int ws_field_semantics(const ws_handle* handle,uint32_t field_id,ws_field_semantics_v1* out_semantics) {
    if (!handle || !out_semantics) return 0;
    try {
        const auto semantics=handle->sim->fields().descriptor(field_id).semantics;
        *out_semantics=semantics==FieldSemantics::Extensive ? WS_FIELD_EXTENSIVE_V1 : WS_FIELD_INTENSIVE_V1;
        handle->last_error.clear();
        return 1;
    } catch (const std::exception& e) { handle->last_error=e.what(); return 0; }
}

size_t ws_copy_field_values(const ws_handle* handle,uint32_t field_id,double* out_values,size_t capacity) {
    if (!handle || !out_values) return 0;
    try {
        (void)handle->sim->fields().descriptor(field_id);
        const auto& fs=handle->sim->world().stores().get<FieldStore>();
        size_t i=0;
        for (CellId c:handle->sim->world().active_cells()) {
            if (i>=capacity) break;
            out_values[i++]=fs.get(c,field_id);
        }
        return i;
    } catch (const std::exception& e) {
        handle->last_error=e.what();
        return 0;
    }
}

int ws_schedule_field_impulse(ws_handle* handle,uint64_t tick,uint64_t cell_id,uint32_t field_id,double delta) {
    return guard(handle,[&]{ handle->sim->schedule_field_impulse(tick,CellId(cell_id),field_id,delta); });
}

size_t ws_event_count(const ws_handle* handle) {
    return handle ? handle->sim->world().pending_events().size() : 0U;
}

size_t ws_copy_events(const ws_handle* handle,ws_event_v1* out_events,size_t capacity) {
    if (!handle || !out_events) return 0U;
    const auto& events=handle->sim->world().pending_events();
    const size_t count=std::min(capacity,events.size());
    for (size_t i=0;i<count;++i) {
        const auto& e=events[i];
        out_events[i]={e.tick,e.cell.raw(),e.subject,fnv1a64(e.type),e.magnitude};
    }
    return count;
}

size_t ws_event_type(const ws_handle* handle,size_t index,char* out_text,size_t capacity) {
    if (!handle) return 0U;
    const auto& events=handle->sim->world().pending_events();
    if (index>=events.size()) {
        handle->last_error="invalid event index";
        return 0U;
    }
    handle->last_error.clear();
    return copy_text(events[index].type,out_text,capacity);
}

int ws_clear_events(ws_handle* handle) {
    return guard(handle,[&]{ (void)handle->sim->world().drain_events(); });
}

int ws_save_snapshot_file(const ws_handle* handle,const char* path) {
    if (!handle || !path) return 0;
    try {
        auto data=handle->sim->save_snapshot();
        std::ofstream f(path,std::ios::binary);
        if (!f) throw std::runtime_error("cannot open snapshot for write");
        f.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
        if (!f) throw std::runtime_error("snapshot write failed");
        handle->last_error.clear();
        return 1;
    } catch (const std::exception& e) { handle->last_error=e.what(); return 0; }
}

int ws_load_snapshot_file(ws_handle* handle,const char* path) {
    return guard(handle,[&]{
        std::ifstream f(path,std::ios::binary);
        if (!f) throw std::runtime_error("cannot open snapshot for read");
        std::vector<char> chars((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        std::vector<std::byte> bytes(chars.size());
        std::transform(chars.begin(),chars.end(),bytes.begin(),[](char c){ return static_cast<std::byte>(static_cast<unsigned char>(c)); });
        handle->sim->load_snapshot(bytes);
    });
}

const char* ws_last_error(const ws_handle* handle) { return handle ? handle->last_error.c_str() : "null handle"; }

} // extern C

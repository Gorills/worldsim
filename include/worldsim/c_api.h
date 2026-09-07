#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #if defined(WORLDSIM_C_EXPORTS)
    #define WS_API __declspec(dllexport)
  #else
    #define WS_API __declspec(dllimport)
  #endif
#else
  #define WS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_handle ws_handle;

typedef struct ws_cell_v1 {
    uint64_t cell_id;
    uint8_t level;
    uint8_t _padding[7];
    double x;
    double y;
    double z;
    double area_m2;
} ws_cell_v1;

typedef enum ws_field_semantics_v1 {
    WS_FIELD_INTENSIVE_V1 = 0,
    WS_FIELD_EXTENSIVE_V1 = 1
} ws_field_semantics_v1;

typedef struct ws_event_v1 {
    uint64_t tick;
    uint64_t cell_id;
    uint64_t subject;
    uint64_t type_hash;
    double magnitude;
} ws_event_v1;

WS_API uint32_t ws_abi_version(void);
WS_API ws_handle* ws_create_default(uint64_t seed);
WS_API void ws_destroy(ws_handle* handle);
WS_API int ws_step(ws_handle* handle, uint64_t ticks);
WS_API uint64_t ws_tick(const ws_handle* handle);
WS_API int ws_set_focus(ws_handle* handle, double x, double y, double z);
WS_API int ws_clear_focus(ws_handle* handle);
WS_API size_t ws_cell_count(const ws_handle* handle);
WS_API size_t ws_copy_cells(const ws_handle* handle, ws_cell_v1* out_cells, size_t capacity);
WS_API size_t ws_field_count(const ws_handle* handle);
WS_API int ws_find_field(const ws_handle* handle, const char* key, uint32_t* out_field_id);
/* Returns the required buffer size including the trailing NUL. A NULL output buffer is allowed. */
WS_API size_t ws_field_key(const ws_handle* handle, uint32_t field_id, char* out_text, size_t capacity);
WS_API size_t ws_field_unit(const ws_handle* handle, uint32_t field_id, char* out_text, size_t capacity);
WS_API int ws_field_semantics(const ws_handle* handle, uint32_t field_id, ws_field_semantics_v1* out_semantics);
WS_API size_t ws_copy_field_values(const ws_handle* handle, uint32_t field_id, double* out_values, size_t capacity);
WS_API int ws_schedule_field_impulse(ws_handle* handle, uint64_t tick, uint64_t cell_id, uint32_t field_id, double delta);
WS_API size_t ws_event_count(const ws_handle* handle);
WS_API size_t ws_copy_events(const ws_handle* handle, ws_event_v1* out_events, size_t capacity);
/* Returns the required buffer size including the trailing NUL for the event at index. */
WS_API size_t ws_event_type(const ws_handle* handle, size_t index, char* out_text, size_t capacity);
WS_API int ws_clear_events(ws_handle* handle);
WS_API int ws_save_snapshot_file(const ws_handle* handle, const char* path);
WS_API int ws_load_snapshot_file(ws_handle* handle, const char* path);
WS_API const char* ws_last_error(const ws_handle* handle);

#ifdef __cplusplus
}
#endif

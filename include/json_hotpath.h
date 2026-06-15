#pragma once

#include <stdint.h>
#include <stddef.h>

#define JSON_HOTPATH_ALIGNMENT 8

// Arena allocator for zero-allocation parsing
typedef struct {
    uint8_t *base;
    size_t capacity;
    size_t offset;
} json_arena_t;

void json_arena_init(json_arena_t *a, void *buf, size_t size);
void *json_arena_alloc(json_arena_t *a, size_t size);
void json_arena_reset(json_arena_t *a);

// JSON Value Representation
typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_INT,
    JSON_FLOAT,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

struct json_kv;
struct json_value;

typedef struct json_value {
    json_type_t type;
    union {
        int64_t i;
        double f;
        int b;
        struct { const char *ptr; size_t len; } str; // Zero-copy pointer to raw input
        struct { struct json_value **items; size_t count; size_t capacity; } arr;
        struct { struct json_kv *entries; size_t count; size_t capacity; } obj;
    };
} json_value_t;

typedef struct json_kv {
    const char *key;
    size_t key_len;
    json_value_t *value;
} json_kv_t;

// JSON Parser
typedef struct {
    const char *input;
    size_t input_len;
    size_t pos;
    json_arena_t *arena;
    char error[256];
    int error_code;
} json_parser_t;

int json_parse(json_parser_t *p, const char *input, size_t len, json_arena_t *arena, json_value_t **out);

// High-speed accessors
json_value_t *json_obj_get(json_value_t *obj, const char *key);
json_value_t *json_obj_getn(json_value_t *obj, const char *key, size_t key_len);
int64_t json_get_int(json_value_t *v, int64_t fallback);
double json_get_float(json_value_t *v, double fallback);
const char *json_get_str(json_value_t *v, size_t *out_len);
int json_get_bool(json_value_t *v, int fallback);
size_t json_array_len(json_value_t *v);
json_value_t *json_array_get(json_value_t *v, size_t index);

// JSON Writer / Serializer
typedef struct {
    char *buf;
    size_t capacity;
    size_t len;
    int error;
    
    // Track comma insertion at various nesting depths
    // Bit 0 = we need a comma at depth 0, Bit 1 = at depth 1, etc.
    // Since depth is small for RPC (usually < 16), uint32_t is enough.
    uint32_t need_comma_mask;
    int depth;
} json_writer_t;

void json_writer_init(json_writer_t *w, char *buf, size_t capacity);
size_t json_writer_finish(json_writer_t *w);

void json_write_null(json_writer_t *w);
void json_write_bool(json_writer_t *w, int val);
void json_write_int(json_writer_t *w, int64_t val);
void json_write_float(json_writer_t *w, double val);
void json_write_str(json_writer_t *w, const char *s, size_t len);

void json_write_obj_start(json_writer_t *w);
void json_write_obj_key(json_writer_t *w, const char *key, size_t key_len);
void json_write_obj_end(json_writer_t *w);

void json_write_arr_start(json_writer_t *w);
void json_write_arr_end(json_writer_t *w);

int json_serialize(json_value_t *val, char *buf, size_t buf_len, size_t *out_len);

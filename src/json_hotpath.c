#include "json_hotpath.h"
#include "brpc_prof.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// --- Arena Allocator ---
void json_arena_init(json_arena_t *a, void *buf, size_t size) {
    a->base = (uint8_t *)buf;
    a->capacity = size;
    a->offset = 0;
}

void *json_arena_alloc(json_arena_t *a, size_t size) {
    // 8-byte alignment
    size_t aligned_size = (size + (JSON_HOTPATH_ALIGNMENT - 1)) & ~(JSON_HOTPATH_ALIGNMENT - 1);
    if (a->offset + aligned_size > a->capacity) {
        return NULL;
    }
    void *ptr = a->base + a->offset;
    a->offset += aligned_size;
    return ptr;
}

void json_arena_reset(json_arena_t *a) {
    a->offset = 0;
}

// --- Whitespace Skip Table ---
static const uint8_t ws_table[256] = {
    [' '] = 1, ['\t'] = 1, ['\n'] = 1, ['\r'] = 1
};

#define SKIP_WS(p) while ((p)->pos < (p)->input_len && ws_table[(uint8_t)(p)->input[(p)->pos]]) { (p)->pos++; }

// --- Helpers ---
static void set_error(json_parser_t *p, int code, const char *fmt, ...) {
    p->error_code = code;
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->error, sizeof(p->error), fmt, args);
    va_end(args);
}

// Forward declarations of parser functions
static json_value_t *parse_value(json_parser_t *p);

// Unescape helper for string parsing
static int unescape_string(const char *src, size_t src_len, char *dest, size_t *out_len) {
    size_t d = 0;
    for (size_t s = 0; s < src_len; s++) {
        if (src[s] == '\\') {
            s++;
            if (s >= src_len) return -1;
            switch (src[s]) {
                case '"':  dest[d++] = '"';  break;
                case '\\': dest[d++] = '\\'; break;
                case '/':  dest[d++] = '/';  break;
                case 'b':  dest[d++] = '\b'; break;
                case 'f':  dest[d++] = '\f'; break;
                case 'n':  dest[d++] = '\n'; break;
                case 'r':  dest[d++] = '\r'; break;
                case 't':  dest[d++] = '\t'; break;
                case 'u': {
                    // Expect 4 hex digits (we just parse basic unicode to simplify, or copy raw for now)
                    if (s + 4 >= src_len) return -1;
                    // Simple hex parse
                    uint32_t val = 0;
                    for (int k = 0; k < 4; k++) {
                        char c = src[++s];
                        val <<= 4;
                        if (c >= '0' && c <= '9') val |= (c - '0');
                        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
                        else return -1;
                    }
                    // UTF-8 encode val
                    if (val <= 0x7F) {
                        dest[d++] = (char)val;
                    } else if (val <= 0x7FF) {
                        dest[d++] = (char)(0xC0 | (val >> 6));
                        dest[d++] = (char)(0x80 | (val & 0x3F));
                    } else {
                        dest[d++] = (char)(0xE0 | (val >> 12));
                        dest[d++] = (char)(0x80 | ((val >> 6) & 0x3F));
                        dest[d++] = (char)(0x80 | (val & 0x3F));
                    }
                    break;
                }
                default: return -1;
            }
        } else {
            dest[d++] = src[s];
        }
    }
    *out_len = d;
    return 0;
}

static json_value_t *parse_string(json_parser_t *p) {
    if (p->pos >= p->input_len || p->input[p->pos] != '"') {
        set_error(p, 1, "Expected quote for string at pos %zu", p->pos);
        return NULL;
    }
    p->pos++; // consume starting '"'
    size_t start = p->pos;
    int has_escapes = 0;
    while (p->pos < p->input_len) {
        char c = p->input[p->pos];
        if (c == '"') {
            break;
        } else if (c == '\\') {
            has_escapes = 1;
            p->pos += 2; // skip escape character
        } else {
            p->pos++;
        }
    }

    if (p->pos >= p->input_len || p->input[p->pos] != '"') {
        set_error(p, 2, "Unterminated string at pos %zu", start);
        return NULL;
    }

    size_t raw_len = p->pos - start;
    p->pos++; // consume ending '"'

    json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
    if (!v) {
        set_error(p, 3, "Arena out of memory");
        return NULL;
    }
    v->type = JSON_STRING;

    if (!has_escapes) {
        v->str.ptr = p->input + start;
        v->str.len = raw_len;
    } else {
        char *unescaped = (char *)json_arena_alloc(p->arena, raw_len + 1);
        if (!unescaped) {
            set_error(p, 3, "Arena out of memory for unescaped string");
            return NULL;
        }
        size_t out_len = 0;
        if (unescape_string(p->input + start, raw_len, unescaped, &out_len) != 0) {
            set_error(p, 4, "Invalid escape sequence in string");
            return NULL;
        }
        unescaped[out_len] = '\0';
        v->str.ptr = unescaped;
        v->str.len = out_len;
    }

    return v;
}

static json_value_t *parse_number(json_parser_t *p) {
    size_t start = p->pos;
    int is_float = 0;
    int sign = 1;

    if (p->pos < p->input_len && p->input[p->pos] == '-') {
        sign = -1;
        p->pos++;
    }

    int64_t int_val = 0;
    size_t digits = 0;
    int overflow = 0;
    while (p->pos < p->input_len) {
        char c = p->input[p->pos];
        if (c >= '0' && c <= '9') {
            int64_t next = int_val * 10 + (c - '0');
            if (next < int_val) overflow = 1;
            int_val = next;
            digits++;
            p->pos++;
        } else {
            break;
        }
    }

    if (p->pos < p->input_len) {
        char c = p->input[p->pos];
        if (c == '.' || c == 'e' || c == 'E') {
            is_float = 1;
        }
    }

    json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
    if (!v) {
        set_error(p, 3, "Arena out of memory");
        return NULL;
    }

    if (is_float || overflow || (digits == 1 && int_val == 0 && sign == -1)) {
        char *endptr;
        double f_val = strtod(p->input + start, &endptr);
        p->pos = endptr - p->input;
        v->type = JSON_FLOAT;
        v->f = f_val;
    } else {
        if (digits == 0) {
            set_error(p, 5, "Invalid number format at pos %zu", start);
            return NULL;
        }
        v->type = JSON_INT;
        v->i = sign * int_val;
    }

    return v;
}

static json_value_t *parse_array(json_parser_t *p) {
    p->pos++; // consume '['
    SKIP_WS(p);

    // Initial capacities
    size_t capacity = 8;
    json_value_t **items = (json_value_t **)json_arena_alloc(p->arena, capacity * sizeof(json_value_t *));
    if (!items) {
        set_error(p, 3, "Arena out of memory for array");
        return NULL;
    }

    size_t count = 0;
    if (p->pos < p->input_len && p->input[p->pos] == ']') {
        p->pos++;
        json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
        if (!v) return NULL;
        v->type = JSON_ARRAY;
        v->arr.items = items;
        v->arr.count = 0;
        v->arr.capacity = capacity;
        return v;
    }

    while (p->pos < p->input_len) {
        json_value_t *item = parse_value(p);
        if (!item) return NULL;

        if (count >= capacity) {
            // Resize by allocating new array and copying (bump arena)
            size_t new_cap = capacity * 2;
            json_value_t **new_items = (json_value_t **)json_arena_alloc(p->arena, new_cap * sizeof(json_value_t *));
            if (!new_items) {
                set_error(p, 3, "Arena out of memory for array resize");
                return NULL;
            }
            memcpy(new_items, items, count * sizeof(json_value_t *));
            items = new_items;
            capacity = new_cap;
        }
        items[count++] = item;

        SKIP_WS(p);
        if (p->pos < p->input_len && p->input[p->pos] == ']') {
            p->pos++;
            break;
        } else if (p->pos < p->input_len && p->input[p->pos] == ',') {
            p->pos++;
            SKIP_WS(p);
        } else {
            set_error(p, 6, "Expected ',' or ']' in array at pos %zu", p->pos);
            return NULL;
        }
    }

    json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
    if (!v) return NULL;
    v->type = JSON_ARRAY;
    v->arr.items = items;
    v->arr.count = count;
    v->arr.capacity = capacity;
    return v;
}

static json_value_t *parse_object(json_parser_t *p) {
    p->pos++; // consume '{'
    SKIP_WS(p);

    size_t capacity = 8;
    json_kv_t *entries = (json_kv_t *)json_arena_alloc(p->arena, capacity * sizeof(json_kv_t));
    if (!entries) {
        set_error(p, 3, "Arena out of memory for object");
        return NULL;
    }

    size_t count = 0;
    if (p->pos < p->input_len && p->input[p->pos] == '}') {
        p->pos++;
        json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
        if (!v) return NULL;
        v->type = JSON_OBJECT;
        v->obj.entries = entries;
        v->obj.count = 0;
        v->obj.capacity = capacity;
        return v;
    }

    while (p->pos < p->input_len) {
        SKIP_WS(p);
        json_value_t *key_val = parse_string(p);
        if (!key_val) return NULL;

        SKIP_WS(p);
        if (p->pos >= p->input_len || p->input[p->pos] != ':') {
            set_error(p, 7, "Expected ':' after object key at pos %zu", p->pos);
            return NULL;
        }
        p->pos++; // consume ':'
        SKIP_WS(p);

        json_value_t *val = parse_value(p);
        if (!val) return NULL;

        if (count >= capacity) {
            size_t new_cap = capacity * 2;
            json_kv_t *new_entries = (json_kv_t *)json_arena_alloc(p->arena, new_cap * sizeof(json_kv_t));
            if (!new_entries) {
                set_error(p, 3, "Arena out of memory for object resize");
                return NULL;
            }
            memcpy(new_entries, entries, count * sizeof(json_kv_t));
            entries = new_entries;
            capacity = new_cap;
        }

        entries[count].key = key_val->str.ptr;
        entries[count].key_len = key_val->str.len;
        entries[count].value = val;
        count++;

        SKIP_WS(p);
        if (p->pos < p->input_len && p->input[p->pos] == '}') {
            p->pos++;
            break;
        } else if (p->pos < p->input_len && p->input[p->pos] == ',') {
            p->pos++;
            SKIP_WS(p);
        } else {
            set_error(p, 8, "Expected ',' or '}' in object at pos %zu", p->pos);
            return NULL;
        }
    }

    json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
    if (!v) return NULL;
    v->type = JSON_OBJECT;
    v->obj.entries = entries;
    v->obj.count = count;
    v->obj.capacity = capacity;
    return v;
}

static int is_id_continue(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '_';
}

static json_value_t *parse_value(json_parser_t *p) {
    SKIP_WS(p);
    if (p->pos >= p->input_len) {
        set_error(p, 9, "Unexpected end of input");
        return NULL;
    }

    char c = p->input[p->pos];
    if (c == '"') {
        return parse_string(p);
    } else if (c == '{') {
        return parse_object(p);
    } else if (c == '[') {
        return parse_array(p);
    } else if ((c >= '0' && c <= '9') || c == '-') {
        return parse_number(p);
    } else if (p->pos + 4 <= p->input_len && memcmp(p->input + p->pos, "true", 4) == 0 &&
               (p->pos + 4 >= p->input_len || !is_id_continue(p->input[p->pos + 4]))) {
        p->pos += 4;
        json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
        if (!v) return NULL;
        v->type = JSON_BOOL;
        v->b = 1;
        return v;
    } else if (p->pos + 5 <= p->input_len && memcmp(p->input + p->pos, "false", 5) == 0 &&
               (p->pos + 5 >= p->input_len || !is_id_continue(p->input[p->pos + 5]))) {
        p->pos += 5;
        json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
        if (!v) return NULL;
        v->type = JSON_BOOL;
        v->b = 0;
        return v;
    } else if (p->pos + 4 <= p->input_len && memcmp(p->input + p->pos, "null", 4) == 0 &&
               (p->pos + 4 >= p->input_len || !is_id_continue(p->input[p->pos + 4]))) {
        p->pos += 4;
        json_value_t *v = (json_value_t *)json_arena_alloc(p->arena, sizeof(json_value_t));
        if (!v) return NULL;
        v->type = JSON_NULL;
        return v;
    }

    set_error(p, 10, "Unexpected token '%c' at pos %zu", c, p->pos);
    return NULL;
}

int json_parse(json_parser_t *p, const char *input, size_t len, json_arena_t *arena, json_value_t **out) {
    uint64_t _t0 = BRPC_PROF_NOW();

    p->input = input;
    p->input_len = len;
    p->pos = 0;
    p->arena = arena;
    p->error[0] = '\0';
    p->error_code = 0;

    json_value_t *root = parse_value(p);
    if (!root) {
        return -1;
    }
    
    SKIP_WS(p);
    if (p->pos < p->input_len) {
        set_error(p, 11, "Extra data after valid JSON at pos %zu", p->pos);
        return -1;
    }

    *out = root;
    BRPC_PROF_RECORD("json_parse", BRPC_PROF_NOW() - _t0, len);
    return 0;
}

// --- Accessors ---
json_value_t *json_obj_getn(json_value_t *obj, const char *key, size_t key_len) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->obj.count; i++) {
        if (obj->obj.entries[i].key_len == key_len &&
            memcmp(obj->obj.entries[i].key, key, key_len) == 0) {
            return obj->obj.entries[i].value;
        }
    }
    return NULL;
}

json_value_t *json_obj_get(json_value_t *obj, const char *key) {
    return json_obj_getn(obj, key, strlen(key));
}

int64_t json_get_int(json_value_t *v, int64_t fallback) {
    if (!v) return fallback;
    if (v->type == JSON_INT) return v->i;
    if (v->type == JSON_FLOAT) return (int64_t)v->f;
    return fallback;
}

double json_get_float(json_value_t *v, double fallback) {
    if (!v) return fallback;
    if (v->type == JSON_FLOAT) return v->f;
    if (v->type == JSON_INT) return (double)v->i;
    return fallback;
}

const char *json_get_str(json_value_t *v, size_t *out_len) {
    if (!v || v->type != JSON_STRING) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = v->str.len;
    return v->str.ptr;
}

int json_get_bool(json_value_t *v, int fallback) {
    if (!v) return fallback;
    if (v->type == JSON_BOOL) return v->b;
    return fallback;
}

size_t json_array_len(json_value_t *v) {
    if (!v || v->type != JSON_ARRAY) return 0;
    return v->arr.count;
}

json_value_t *json_array_get(json_value_t *v, size_t index) {
    if (!v || v->type != JSON_ARRAY || index >= v->arr.count) return NULL;
    return v->arr.items[index];
}

// --- Serializer / Writer ---
void json_writer_init(json_writer_t *w, char *buf, size_t capacity) {
    w->buf = buf;
    w->capacity = capacity;
    w->len = 0;
    w->error = 0;
    w->need_comma_mask = 0;
    w->depth = 0;
}

size_t json_writer_finish(json_writer_t *w) {
    if (w->error) return 0;
    if (w->len < w->capacity) {
        w->buf[w->len] = '\0';
    }
    return w->len;
}

static void write_raw(json_writer_t *w, const char *s, size_t len) {
    if (w->error) return;
    if (w->len + len >= w->capacity) {
        w->error = 1;
        return;
    }
    memcpy(w->buf + w->len, s, len);
    w->len += len;
}

static void write_comma_if_needed(json_writer_t *w) {
    if (w->depth > 0 && w->depth < 32) {
        if (w->need_comma_mask & (1U << w->depth)) {
            write_raw(w, ",", 1);
        } else {
            w->need_comma_mask |= (1U << w->depth);
        }
    }
}

void json_write_null(json_writer_t *w) {
    write_comma_if_needed(w);
    write_raw(w, "null", 4);
}

void json_write_bool(json_writer_t *w, int val) {
    write_comma_if_needed(w);
    if (val) {
        write_raw(w, "true", 4);
    } else {
        write_raw(w, "false", 5);
    }
}

void json_write_int(json_writer_t *w, int64_t val) {
    write_comma_if_needed(w);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)val);
    write_raw(w, buf, len);
}

void json_write_float(json_writer_t *w, double val) {
    write_comma_if_needed(w);
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%.17g", val);
    write_raw(w, buf, len);
}

static void write_escaped_str(json_writer_t *w, const char *s, size_t len) {
    write_raw(w, "\"", 1);
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        switch (c) {
            case '"':  write_raw(w, "\\\"", 2); break;
            case '\\': write_raw(w, "\\\\", 2); break;
            case '\b': write_raw(w, "\\b", 2);  break;
            case '\f': write_raw(w, "\\f", 2);  break;
            case '\n': write_raw(w, "\\n", 2);  break;
            case '\r': write_raw(w, "\\r", 2);  break;
            case '\t': write_raw(w, "\\t", 2);  break;
            default: {
                if (c < 0x20) {
                    char buf[8];
                    int hex_len = snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
                    write_raw(w, buf, hex_len);
                } else {
                    write_raw(w, &c, 1);
                }
            }
        }
    }
    write_raw(w, "\"", 1);
}

void json_write_str(json_writer_t *w, const char *s, size_t len) {
    write_comma_if_needed(w);
    write_escaped_str(w, s, len);
}

void json_write_obj_start(json_writer_t *w) {
    write_comma_if_needed(w);
    write_raw(w, "{", 1);
    w->depth++;
    if (w->depth < 32) {
        w->need_comma_mask &= ~(1U << w->depth); // Clear comma flag for next level
    }
}

void json_write_obj_key(json_writer_t *w, const char *key, size_t key_len) {
    if (w->depth < 32) {
        if (w->need_comma_mask & (1U << w->depth)) {
            write_raw(w, ",", 1);
        } else {
            w->need_comma_mask |= (1U << w->depth);
        }
    }
    write_escaped_str(w, key, key_len);
    write_raw(w, ":", 1);
    // Don't need comma for the value itself
    if (w->depth + 1 < 32) {
        w->need_comma_mask &= ~(1U << (w->depth + 1));
    }
    w->depth++; // momentarily enter depth + 1 to write the value without triggering object comma
}

void json_write_obj_end(json_writer_t *w) {
    w->depth--;
    write_raw(w, "}", 1);
}

void json_write_arr_start(json_writer_t *w) {
    write_comma_if_needed(w);
    write_raw(w, "[", 1);
    w->depth++;
    if (w->depth < 32) {
        w->need_comma_mask &= ~(1U << w->depth);
    }
}

void json_write_arr_end(json_writer_t *w) {
    w->depth--;
    write_raw(w, "]", 1);
}

static void serialize_value(json_writer_t *w, json_value_t *v) {
    if (!v) {
        json_write_null(w);
        return;
    }
    switch (v->type) {
        case JSON_NULL:
            json_write_null(w);
            break;
        case JSON_BOOL:
            json_write_bool(w, v->b);
            break;
        case JSON_INT:
            json_write_int(w, v->i);
            break;
        case JSON_FLOAT:
            json_write_float(w, v->f);
            break;
        case JSON_STRING:
            json_write_str(w, v->str.ptr, v->str.len);
            break;
        case JSON_ARRAY:
            json_write_arr_start(w);
            for (size_t i = 0; i < v->arr.count; i++) {
                serialize_value(w, v->arr.items[i]);
            }
            json_write_arr_end(w);
            break;
        case JSON_OBJECT:
            json_write_obj_start(w);
            for (size_t i = 0; i < v->obj.count; i++) {
                json_write_obj_key(w, v->obj.entries[i].key, v->obj.entries[i].key_len);
                // The obj_key function increments depth by 1. We serialize value and decrement depth
                serialize_value(w, v->obj.entries[i].value);
                w->depth--;
            }
            json_write_obj_end(w);
            break;
    }
}

int json_serialize(json_value_t *val, char *buf, size_t buf_len, size_t *out_len) {
    uint64_t _t0 = BRPC_PROF_NOW();

    json_writer_t w;
    json_writer_init(&w, buf, buf_len);
    serialize_value(&w, val);
    size_t written = json_writer_finish(&w);
    if (w.error) {
        return -1;
    }
    if (out_len) {
        *out_len = written;
    }
    BRPC_PROF_RECORD("json_serialize", BRPC_PROF_NOW() - _t0, written);
    return 0;
}

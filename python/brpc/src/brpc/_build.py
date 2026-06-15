"""
bRPC CFFI bindings — ABI-level (dlopen) to avoid struct layout issues.
"""
import os
import cffi

ffibuilder = cffi.FFI()

ffibuilder.cdef("""
/* JSON Arena */
typedef struct { unsigned char *base; size_t capacity; size_t offset; } json_arena_t;
void json_arena_init(json_arena_t *a, void *buf, size_t size);
void json_arena_reset(json_arena_t *a);

/* JSON Parser — opaque, don't touch fields directly */
typedef struct { ...; } json_parser_t;
int json_parse(json_parser_t *p, const char *input, size_t len,
               json_arena_t *arena, void **out);

/* JSON Value — opaque */
typedef struct { ...; } json_value_t;
void *json_obj_get(json_value_t *obj, const char *key);
long long json_get_int(json_value_t *v, long long fallback);
double json_get_float(json_value_t *v, double fallback);
const char *json_get_str(json_value_t *v, size_t *out_len);
int json_get_bool(json_value_t *v, int fallback);
size_t json_array_len(json_value_t *v);
void *json_array_get(json_value_t *v, size_t index);

/* JSON Writer — opaque */
typedef struct { ...; } json_writer_t;
void json_writer_init(json_writer_t *w, char *buf, size_t capacity);
size_t json_writer_finish(json_writer_t *w);
void json_write_null(json_writer_t *w);
void json_write_bool(json_writer_t *w, int val);
void json_write_int(json_writer_t *w, long long val);
void json_write_float(json_writer_t *w, double val);
void json_write_str(json_writer_t *w, const char *s, size_t len);
void json_write_obj_start(json_writer_t *w);
void json_write_obj_key(json_writer_t *w, const char *key, size_t key_len);
void json_write_obj_end(json_writer_t *w);
void json_write_arr_start(json_writer_t *w);
void json_write_arr_end(json_writer_t *w);
int json_serialize(json_value_t *val, char *buf, size_t buf_len, size_t *out_len);

/* Frame */
typedef struct { ...; } brpc_frame_t;
int brpc_frame_encode(const brpc_frame_t *frame, unsigned char *buf, size_t buf_len);
int brpc_frame_decode(const unsigned char *buf, size_t buf_len, brpc_frame_t *frame_out);

/* Stream — opaque */
typedef struct { ...; } brpc_stream_t;
int brpc_stream_init(brpc_stream_t *s, unsigned int stream_id, size_t buf_size);
void brpc_stream_destroy(brpc_stream_t *s);
int brpc_stream_write(brpc_stream_t *s, const unsigned char *data, size_t len);
int brpc_stream_read(brpc_stream_t *s, unsigned char *buf, size_t buf_len);
void brpc_stream_close(brpc_stream_t *s);
size_t brpc_stream_available_read(const brpc_stream_t *s);
size_t brpc_stream_available_write(const brpc_stream_t *s);

/* Channel — opaque */
typedef struct { ...; } brpc_channel_t;
int brpc_channel_init(brpc_channel_t *ch, int fd, int is_server);
void brpc_channel_destroy(brpc_channel_t *ch);
void *brpc_channel_open_stream(brpc_channel_t *ch);
void *brpc_channel_find_stream(brpc_channel_t *ch, unsigned int stream_id);
int brpc_channel_send_data(brpc_channel_t *ch, unsigned int stream_id,
                           const unsigned char *data, size_t len, int end_stream);
int brpc_channel_send_ping(brpc_channel_t *ch);
int brpc_channel_send_goaway(brpc_channel_t *ch, unsigned int last_stream_id,
                             unsigned int error_code);
int brpc_channel_close(brpc_channel_t *ch);
int brpc_channel_recv(brpc_channel_t *ch);
int brpc_channel_pump(brpc_channel_t *ch);

/* Profiling */
void brpc_prof_init(void);
void brpc_prof_reset(void);
void brpc_prof_set_enabled(int enabled);
void brpc_prof_print(void);
""")

# Source files
this_dir = os.path.dirname(os.path.abspath(__file__))
root_dir = os.path.join(this_dir, "..", "..", "..", "..")
src_dir = os.path.join(root_dir, "src")
include_dir = os.path.join(root_dir, "include")

source_files = ["json_hotpath.c", "brpc_frame.c", "brpc_stream.c", "brpc_channel.c", "brpc_prof.c"]
combined_source = ""
for f in source_files:
    path = os.path.join(src_dir, f)
    if not os.path.exists(path):
        raise FileNotFoundError(f"Source file not found: {path}")
    with open(path) as fh:
        combined_source += fh.read() + "\n"

ffibuilder.set_source(
    "brpc._brpc",
    combined_source,
    include_dirs=[include_dir],
    libraries=[],
)

if __name__ == "__main__":
    ffibuilder.compile(verbose=True)

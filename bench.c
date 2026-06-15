/**
 * bench.c — brpc performance benchmarks
 *
 * Measures:
 *   1. JSON parse/serialize throughput (messages/sec)
 *   2. Frame encode/decode throughput
 *   3. Stream write/read throughput
 *   4. Channel round-trip latency
 *   5. Memory usage per parse
 *
 * Build: zig cc -O2 -Iinclude -o bench bench.c src/json_hotpath.c src/brpc_frame.c src/brpc_stream.c src/brpc_channel.c src/brpc_prof.c -lm
 * Run:   ./bench
 */
#include "json_hotpath.h"
#include "brpc_frame.h"
#include "brpc_stream.h"
#include "brpc_channel.h"
#include "brpc_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/resource.h>

/* ── Timing ──────────────────────────────────────────────────────────── */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Memory measurement ──────────────────────────────────────────────── */

static size_t get_peak_rss_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (size_t)ru.ru_maxrss;  /* KB on Linux */
}

/* ── Benchmark runner ────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    double avg_us;
    double min_us;
    double max_us;
    double p50_us;
    double p99_us;
    double throughput;
    size_t iterations;
} bench_result_t;

static int cmp_u64(const void *a, const void *b) {
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static bench_result_t run_bench(const char *name, void (*fn)(void),
                                size_t iterations, size_t warmup) {
    uint64_t *times = (uint64_t *)malloc(iterations * sizeof(uint64_t));

    for (size_t i = 0; i < warmup; i++) fn();

    for (size_t i = 0; i < iterations; i++) {
        uint64_t t0 = now_ns();
        fn();
        uint64_t t1 = now_ns();
        times[i] = t1 - t0;
    }

    qsort(times, iterations, sizeof(uint64_t), cmp_u64);

    bench_result_t r = {0};
    r.name = name;
    r.iterations = iterations;

    uint64_t total = 0;
    for (size_t i = 0; i < iterations; i++) total += times[i];
    r.avg_us = (double)total / (double)iterations / 1000.0;
    r.min_us = (double)times[0] / 1000.0;
    r.max_us = (double)times[iterations - 1] / 1000.0;
    r.p50_us = (double)times[iterations / 2] / 1000.0;
    r.p99_us = (double)times[(size_t)(iterations * 0.99)] / 1000.0;
    r.throughput = r.avg_us > 0 ? 1000000.0 / r.avg_us : 0;

    free(times);
    return r;
}

static void print_result(const bench_result_t *r) {
    printf("  %-35s %8zu iters  avg=%8.1fus  min=%8.1fus  p50=%8.1fus  p99=%8.1fus  max=%8.1fus  %10.0f/s\n",
           r->name, r->iterations, r->avg_us, r->min_us, r->p50_us,
           r->p99_us, r->max_us, r->throughput);
}

/* ── Test payloads ───────────────────────────────────────────────────── */

static const char *SMALL_JSON  = "{\"method\":\"ping\",\"id\":1}";
static const char *MEDIUM_JSON = "{\"jsonrpc\":\"2.0\",\"method\":\"getUser\",\"id\":42,\"params\":{\"userId\":42,\"includeProfile\":true,\"fields\":[\"name\",\"email\",\"avatar\"]}}";
static const char *LARGE_JSON  = "{\"users\":[{\"id\":1,\"name\":\"Alice\",\"email\":\"alice@example.com\",\"tags\":[\"admin\",\"active\"],\"score\":99.5},{\"id\":2,\"name\":\"Bob\",\"email\":\"bob@example.com\",\"tags\":[\"user\"],\"score\":87.3},{\"id\":3,\"name\":\"Charlie\",\"email\":\"charlie@example.com\",\"tags\":[\"user\",\"moderator\"],\"score\":92.1},{\"id\":4,\"name\":\"Diana\",\"email\":\"diana@example.com\",\"tags\":[\"admin\"],\"score\":95.7},{\"id\":5,\"name\":\"Eve\",\"email\":\"eve@example.com\",\"tags\":[\"user\",\"active\"],\"score\":88.9}],\"metadata\":{\"total\":5,\"page\":1,\"hasMore\":false}}";

/* ── JSON Parse Benchmarks ───────────────────────────────────────────── */

static json_arena_t g_arena;
static uint8_t g_arena_buf[65536];
static json_parser_t g_parser;
static json_value_t *g_root;

static void bench_parse_small(void) {
    json_arena_reset(&g_arena);
    json_parse(&g_parser, SMALL_JSON, strlen(SMALL_JSON), &g_arena, &g_root);
}

static void bench_parse_medium(void) {
    json_arena_reset(&g_arena);
    json_parse(&g_parser, MEDIUM_JSON, strlen(MEDIUM_JSON), &g_arena, &g_root);
}

static void bench_parse_large(void) {
    json_arena_reset(&g_arena);
    json_parse(&g_parser, LARGE_JSON, strlen(LARGE_JSON), &g_arena, &g_root);
}

/* ── JSON Serialize Benchmarks ───────────────────────────────────────── */

static char g_wbuf[4096];
static json_writer_t g_writer;

static void bench_serialize_small(void) {
    json_writer_init(&g_writer, g_wbuf, sizeof(g_wbuf));
    json_write_obj_start(&g_writer);
    json_write_obj_key(&g_writer, "method", 6);
    json_write_str(&g_writer, "ping", 4);
    json_write_obj_key(&g_writer, "id", 2);
    json_write_int(&g_writer, 1);
    json_write_obj_end(&g_writer);
    json_writer_finish(&g_writer);
}

static void bench_serialize_medium(void) {
    json_writer_init(&g_writer, g_wbuf, sizeof(g_wbuf));
    json_write_obj_start(&g_writer);
    json_write_obj_key(&g_writer, "jsonrpc", 7);
    json_write_str(&g_writer, "2.0", 3);
    json_write_obj_key(&g_writer, "method", 6);
    json_write_str(&g_writer, "getUser", 7);
    json_write_obj_key(&g_writer, "id", 2);
    json_write_int(&g_writer, 42);
    json_write_obj_key(&g_writer, "params", 6);
    json_write_obj_start(&g_writer);
    json_write_obj_key(&g_writer, "userId", 6);
    json_write_int(&g_writer, 42);
    json_write_obj_key(&g_writer, "includeProfile", 14);
    json_write_bool(&g_writer, 1);
    json_write_obj_end(&g_writer);
    json_write_obj_end(&g_writer);
    json_writer_finish(&g_writer);
}

/* ── Frame Encode/Decode Benchmarks ──────────────────────────────────── */

static uint8_t g_fbuf[1024];
static uint8_t g_fpayload[128];
static brpc_frame_t g_frame;

static void bench_frame_encode(void) {
    g_frame.stream_id = 1;
    g_frame.type = BRPC_FRAME_DATA;
    g_frame.flags = BRPC_FLAG_END_STREAM;
    g_frame.payload_length = sizeof(g_fpayload);
    g_frame.payload = g_fpayload;
    brpc_frame_encode(&g_frame, g_fbuf, sizeof(g_fbuf));
}

static void bench_frame_decode(void) {
    brpc_frame_t out;
    brpc_frame_decode(g_fbuf, 10 + sizeof(g_fpayload), &out);
}

/* ── Stream Write/Read Benchmarks ────────────────────────────────────── */

static brpc_stream_t g_stream;
static uint8_t g_sdata[1024];

static void bench_stream_write(void) {
    brpc_stream_write(&g_stream, g_sdata, sizeof(g_sdata));
}

static void bench_stream_read(void) {
    uint8_t buf[1024];
    brpc_stream_read(&g_stream, buf, sizeof(buf));
}

/* ── Channel Round-trip Benchmark ────────────────────────────────────── */

static brpc_channel_t g_client_ch;
static brpc_channel_t g_server_ch;
static int g_sv[2];
static uint32_t g_stream_id;

static void setup_channel(void) {
    socketpair(AF_UNIX, SOCK_STREAM, 0, g_sv);
    brpc_channel_init(&g_client_ch, g_sv[0], 0, 0);
    brpc_channel_init(&g_server_ch, g_sv[1], 1, 0);
    brpc_stream_t *s = brpc_channel_open_stream(&g_client_ch);
    g_stream_id = s->stream_id;
}

static void teardown_channel(void) {
    brpc_channel_destroy(&g_client_ch);
    brpc_channel_destroy(&g_server_ch);
    close(g_sv[0]);
    close(g_sv[1]);
}

static void bench_channel_roundtrip(void) {
    brpc_channel_send_data(&g_client_ch, g_stream_id,
                           (const uint8_t *)MEDIUM_JSON, strlen(MEDIUM_JSON), 0);
    brpc_channel_pump(&g_server_ch);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("═══════════════════════════════════════════════════════════════════════════════════════\n");
    printf("brpc Performance Benchmarks\n");
    printf("═══════════════════════════════════════════════════════════════════════════════════════\n\n");

    json_arena_init(&g_arena, g_arena_buf, sizeof(g_arena_buf));
    memset(g_fpayload, 'x', sizeof(g_fpayload));
    memset(g_sdata, 'a', sizeof(g_sdata));

    /* ── JSON Parse ────────────────────────────────────── */
    printf("── JSON Parse (arena, zero-copy) ──\n");
    bench_result_t r;
    r = run_bench("json_parse (28B)",  bench_parse_small,  10000, 100); print_result(&r);
    r = run_bench("json_parse (99B)",  bench_parse_medium, 10000, 100); print_result(&r);
    r = run_bench("json_parse (310B)", bench_parse_large,  10000, 100); print_result(&r);
    printf("\n");

    /* ── JSON Serialize ────────────────────────────────── */
    printf("── JSON Serialize (streaming writer) ──\n");
    r = run_bench("json_serialize (small)",  bench_serialize_small,  10000, 100); print_result(&r);
    r = run_bench("json_serialize (medium)", bench_serialize_medium, 10000, 100); print_result(&r);
    printf("\n");

    /* ── Frame Encode/Decode ───────────────────────────── */
    printf("── Frame Encode/Decode (128B payload) ──\n");
    bench_frame_encode();  /* Pre-encode for decode bench */
    r = run_bench("frame_encode (128B)", bench_frame_encode, 10000, 100); print_result(&r);
    r = run_bench("frame_decode (128B)", bench_frame_decode, 10000, 100); print_result(&r);
    printf("\n");

    /* ── Stream Write/Read ─────────────────────────────── */
    printf("── Stream Write/Read (1KB, ring buffer) ──\n");
    brpc_stream_init(&g_stream, 1, 65536);
    /* Fill for read bench */
    for (int i = 0; i < 100; i++) brpc_stream_write(&g_stream, g_sdata, sizeof(g_sdata));
    r = run_bench("stream_write (1KB)", bench_stream_write, 10000, 100); print_result(&r);
    r = run_bench("stream_read (1KB)",  bench_stream_read,  10000, 100); print_result(&r);
    brpc_stream_destroy(&g_stream);
    printf("\n");

    /* ── Channel Round-trip ────────────────────────────── */
    printf("── Channel Round-trip (send+recv, socketpair) ──\n");
    setup_channel();
    r = run_bench("channel_roundtrip", bench_channel_roundtrip, 10000, 100); print_result(&r);
    teardown_channel();
    printf("\n");

    /* ── Memory ────────────────────────────────────────── */
    printf("── Memory ──\n");
    printf("  Peak RSS:                %zu KB\n", get_peak_rss_kb());
    printf("  Arena size (parse):      %zu bytes\n", sizeof(g_arena_buf));
    printf("  sizeof(brpc_stream_t):   %zu bytes\n", sizeof(brpc_stream_t));
    printf("  sizeof(brpc_channel_t):  %zu bytes (streams dynamically allocated)\n", sizeof(brpc_channel_t));
    printf("\n");

    /* ── Profiling Summary ─────────────────────────────── */
    printf("── Internal Profiling ──\n");
    brpc_prof_print();

    printf("═══════════════════════════════════════════════════════════════════════════════════════\n");
    return 0;
}

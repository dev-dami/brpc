#include "json_hotpath.h"
#include "brpc_frame.h"
#include "brpc_stream.h"
#include "brpc_channel.h"
#include "brpc_rpc.h"
#include "brpc_prof.h"
#include "brpc_compress.h"
#include "brpc_tls.h"
#include "brpc_error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { tests_run++; printf("  %-40s ", name); } while(0)
#define PASS() \
    do { tests_passed++; printf("[PASS]\n"); } while(0)
#define FAIL(msg) \
    do { printf("[FAIL] %s\n", msg); } while(0)

/* ========================================================================
 * JSON Parser Tests
 * ======================================================================== */

static void test_json_primitives(void) {
    uint8_t arena_buf[4096];
    json_arena_t arena;
    json_arena_init(&arena, arena_buf, sizeof(arena_buf));
    json_parser_t p;
    json_value_t *root;

    TEST("parse null");
    if (json_parse(&p, "null", 4, &arena, &root) == 0 &&
        root->type == JSON_NULL) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse true");
    json_arena_reset(&arena);
    if (json_parse(&p, "true", 4, &arena, &root) == 0 &&
        root->type == JSON_BOOL && root->b == 1) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse false");
    json_arena_reset(&arena);
    if (json_parse(&p, "false", 5, &arena, &root) == 0 &&
        root->type == JSON_BOOL && root->b == 0) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse integer");
    json_arena_reset(&arena);
    if (json_parse(&p, "42", 2, &arena, &root) == 0 &&
        root->type == JSON_INT && root->i == 42) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse negative integer");
    json_arena_reset(&arena);
    if (json_parse(&p, "-123", 4, &arena, &root) == 0 &&
        root->type == JSON_INT && root->i == -123) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse float");
    json_arena_reset(&arena);
    if (json_parse(&p, "3.14", 4, &arena, &root) == 0 &&
        root->type == JSON_FLOAT && root->f > 3.13 && root->f < 3.15) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse scientific notation");
    json_arena_reset(&arena);
    if (json_parse(&p, "1e10", 4, &arena, &root) == 0 &&
        root->type == JSON_FLOAT && root->f == 1e10) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse -0 as float");
    json_arena_reset(&arena);
    if (json_parse(&p, "-0", 2, &arena, &root) == 0 &&
        root->type == JSON_FLOAT && root->f == 0.0) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse string");
    json_arena_reset(&arena);
    if (json_parse(&p, "\"hello\"", 7, &arena, &root) == 0 &&
        root->type == JSON_STRING && root->str.len == 5 &&
        memcmp(root->str.ptr, "hello", 5) == 0) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse escaped string");
    json_arena_reset(&arena);
    if (json_parse(&p, "\"a\\nb\"", 6, &arena, &root) == 0 &&
        root->type == JSON_STRING && root->str.len == 3 &&
        root->str.ptr[0] == 'a' && root->str.ptr[1] == '\n' &&
        root->str.ptr[2] == 'b') {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse unicode escape");
    json_arena_reset(&arena);
    if (json_parse(&p, "\"\\u0041\"", 8, &arena, &root) == 0 &&
        root->type == JSON_STRING && root->str.len == 1 &&
        root->str.ptr[0] == 'A') {
        PASS();
    } else { FAIL(p.error); }
}

static void test_json_containers(void) {
    uint8_t arena_buf[4096];
    json_arena_t arena;
    json_arena_init(&arena, arena_buf, sizeof(arena_buf));
    json_parser_t p;
    json_value_t *root;

    TEST("parse empty array");
    if (json_parse(&p, "[]", 2, &arena, &root) == 0 &&
        root->type == JSON_ARRAY && root->arr.count == 0) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse array with elements");
    json_arena_reset(&arena);
    if (json_parse(&p, "[1,2,3]", 7, &arena, &root) == 0 &&
        root->type == JSON_ARRAY && root->arr.count == 3 &&
        json_array_get(root, 0)->i == 1 &&
        json_array_get(root, 1)->i == 2 &&
        json_array_get(root, 2)->i == 3) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse nested array");
    json_arena_reset(&arena);
    if (json_parse(&p, "[[1],[2,3]]", 11, &arena, &root) == 0 &&
        root->type == JSON_ARRAY && root->arr.count == 2 &&
        json_array_get(root, 0)->arr.count == 1 &&
        json_array_get(root, 1)->arr.count == 2) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse empty object");
    json_arena_reset(&arena);
    if (json_parse(&p, "{}", 2, &arena, &root) == 0 &&
        root->type == JSON_OBJECT && root->obj.count == 0) {
        PASS();
    } else { FAIL(p.error); }

    TEST("parse object with key-value pairs");
    json_arena_reset(&arena);
    const char *json = "{\"name\":\"test\",\"value\":42}";
    if (json_parse(&p, json, strlen(json), &arena, &root) == 0 &&
        root->type == JSON_OBJECT && root->obj.count == 2) {
        json_value_t *name = json_obj_get(root, "name");
        json_value_t *value = json_obj_get(root, "value");
        if (name && name->type == JSON_STRING &&
            value && value->type == JSON_INT && value->i == 42) {
            PASS();
        } else { FAIL("key lookup failed"); }
    } else { FAIL(p.error); }

    TEST("parse nested object");
    json_arena_reset(&arena);
    const char *nested = "{\"a\":{\"b\":1},\"c\":2}";
    if (json_parse(&p, nested, strlen(nested), &arena, &root) == 0 &&
        root->type == JSON_OBJECT) {
        json_value_t *a = json_obj_get(root, "a");
        if (a && a->type == JSON_OBJECT) {
            json_value_t *b = json_obj_get(a, "b");
            if (b && b->type == JSON_INT && b->i == 1) {
                PASS();
            } else { FAIL("nested lookup failed"); }
        } else { FAIL("inner object missing"); }
    } else { FAIL(p.error); }
}

static void test_json_accessors(void) {
    uint8_t arena_buf[4096];
    json_arena_t arena;
    json_arena_init(&arena, arena_buf, sizeof(arena_buf));
    json_parser_t p;
    json_value_t *root;

    const char *json = "{\"name\":\"hello\",\"count\":5,\"pi\":3.14,\"ok\":true,\"data\":null}";
    json_parse(&p, json, strlen(json), &arena, &root);

    TEST("json_get_str");
    size_t slen;
    const char *s = json_get_str(json_obj_get(root, "name"), &slen);
    if (s && slen == 5 && memcmp(s, "hello", 5) == 0) {
        PASS();
    } else { FAIL("wrong string"); }

    TEST("json_get_int");
    if (json_get_int(json_obj_get(root, "count"), 0) == 5) {
        PASS();
    } else { FAIL("wrong int"); }

    TEST("json_get_float");
    double f = json_get_float(json_obj_get(root, "pi"), 0);
    if (f > 3.13 && f < 3.15) {
        PASS();
    } else { FAIL("wrong float"); }

    TEST("json_get_bool");
    if (json_get_bool(json_obj_get(root, "ok"), 0) == 1) {
        PASS();
    } else { FAIL("wrong bool"); }

    TEST("json_obj_getn length-delimited");
    json_value_t *v = json_obj_getn(root, "name", 4);
    if (v && v->type == JSON_STRING) {
        PASS();
    } else { FAIL("getn failed"); }

    TEST("json_array_len on null");
    if (json_array_len(NULL) == 0) {
        PASS();
    } else { FAIL("should return 0"); }

    TEST("fallback values");
    if (json_get_int(NULL, -1) == -1 &&
        json_get_float(NULL, -1.0) == -1.0 &&
        json_get_bool(NULL, -1) == -1 &&
        json_get_str(NULL, NULL) == NULL) {
        PASS();
    } else { FAIL("fallback failed"); }
}

static void test_json_errors(void) {
    uint8_t arena_buf[4096];
    json_arena_t arena;
    json_arena_init(&arena, arena_buf, sizeof(arena_buf));
    json_parser_t p;
    json_value_t *root;

    TEST("reject unterminated string");
    if (json_parse(&p, "\"abc", 4, &arena, &root) != 0) {
        PASS();
    } else { FAIL("should fail"); }

    TEST("reject extra data");
    json_arena_reset(&arena);
    if (json_parse(&p, "1 2", 3, &arena, &root) != 0) {
        PASS();
    } else { FAIL("should fail"); }

    TEST("reject invalid token");
    json_arena_reset(&arena);
    if (json_parse(&p, "x", 1, &arena, &root) != 0) {
        PASS();
    } else { FAIL("should fail"); }

    TEST("reject missing colon");
    json_arena_reset(&arena);
    if (json_parse(&p, "{\"a\" 1}", 7, &arena, &root) != 0) {
        PASS();
    } else { FAIL("should fail"); }
}

/* ========================================================================
 * JSON Serializer Tests
 * ======================================================================== */

static void test_json_writer(void) {
    char buf[1024];
    json_writer_t w;

    TEST("write null");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_null(&w);
    if (json_writer_finish(&w) == 4 && memcmp(buf, "null", 4) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write bool true");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_bool(&w, 1);
    if (json_writer_finish(&w) == 4 && memcmp(buf, "true", 4) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write bool false");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_bool(&w, 0);
    if (json_writer_finish(&w) == 5 && memcmp(buf, "false", 5) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write int");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_int(&w, -42);
    if (json_writer_finish(&w) == 3 && memcmp(buf, "-42", 3) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write float");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_float(&w, 3.14);
    size_t len = json_writer_finish(&w);
    if (len > 0 && len < sizeof(buf)) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write string");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_str(&w, "hello", 5);
    if (json_writer_finish(&w) == 7 && memcmp(buf, "\"hello\"", 7) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write escaped string");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_str(&w, "a\nb", 3);
    if (json_writer_finish(&w) == 6 && memcmp(buf, "\"a\\nb\"", 6) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write empty object");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_obj_start(&w);
    json_write_obj_end(&w);
    if (json_writer_finish(&w) == 2 && memcmp(buf, "{}", 2) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write object with one key");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_obj_start(&w);
    json_write_obj_key(&w, "key", 3);
    json_write_int(&w, 123);
    json_write_obj_end(&w);
    if (json_writer_finish(&w) == 11 && memcmp(buf, "{\"key\":123}", 11) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write object with two keys");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_obj_start(&w);
    json_write_obj_key(&w, "a", 1);
    json_write_int(&w, 1);
    json_write_obj_key(&w, "b", 1);
    json_write_int(&w, 2);
    json_write_obj_end(&w);
    if (json_writer_finish(&w) == 13 && memcmp(buf, "{\"a\":1,\"b\":2}", 13) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write empty array");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_arr_start(&w);
    json_write_arr_end(&w);
    if (json_writer_finish(&w) == 2 && memcmp(buf, "[]", 2) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("write array with elements");
    json_writer_init(&w, buf, sizeof(buf));
    json_write_arr_start(&w);
    json_write_int(&w, 1);
    json_write_int(&w, 2);
    json_write_int(&w, 3);
    json_write_arr_end(&w);
    if (json_writer_finish(&w) == 7 && memcmp(buf, "[1,2,3]", 7) == 0) {
        PASS();
    } else { FAIL("wrong output"); }

    TEST("buffer overflow detection");
    json_writer_init(&w, buf, 3);
    json_write_str(&w, "hello", 5);
    if (w.error) {
        PASS();
    } else { FAIL("should detect overflow"); }
}

static void test_json_roundtrip(void) {
    uint8_t arena_buf[8192];
    json_arena_t arena;
    json_arena_init(&arena, arena_buf, sizeof(arena_buf));
    json_parser_t p;
    json_value_t *root;

    const char *input = "{\"name\":\"test\",\"values\":[1,2,3],\"nested\":{\"a\":true,\"b\":null}}";
    char out_buf[1024];
    size_t out_len;

    TEST("roundtrip parse+serialize");
    if (json_parse(&p, input, strlen(input), &arena, &root) == 0 &&
        json_serialize(root, out_buf, sizeof(out_buf), &out_len) == 0) {
        /* Re-parse the output to verify it's valid JSON */
        json_arena_t arena2;
        uint8_t arena_buf2[4096];
        json_arena_init(&arena2, arena_buf2, sizeof(arena_buf2));
        json_parser_t p2;
        json_value_t *root2;
        if (json_parse(&p2, out_buf, out_len, &arena2, &root2) == 0) {
            /* Verify structure */
            json_value_t *name = json_obj_get(root2, "name");
            json_value_t *values = json_obj_get(root2, "values");
            if (name && json_get_str(name, NULL) &&
                values && values->arr.count == 3) {
                PASS();
            } else { FAIL("re-parse mismatch"); }
        } else { FAIL("re-parse failed"); }
    } else { FAIL("initial parse/serialize failed"); }
}

/* ========================================================================
 * Frame Tests
 * ======================================================================== */

static void test_frame_encode_decode(void) {
    uint8_t buf[256];

    TEST("encode/decode DATA frame");
    brpc_frame_t f1 = {
        .stream_id = 1,
        .type = BRPC_FRAME_DATA,
        .flags = BRPC_FLAG_END_STREAM,
        .payload_length = 5,
        .payload = (const uint8_t *)"hello"
    };
    int encoded = brpc_frame_encode(&f1, buf, sizeof(buf));
    if (encoded == 15) {
        brpc_frame_t f2;
        int decoded = brpc_frame_decode(buf, (size_t)encoded, &f2);
        if (decoded == 15 &&
            f2.stream_id == 1 &&
            f2.type == BRPC_FRAME_DATA &&
            f2.flags == BRPC_FLAG_END_STREAM &&
            f2.payload_length == 5 &&
            memcmp(f2.payload, "hello", 5) == 0) {
            PASS();
        } else { FAIL("decode mismatch"); }
    } else { FAIL("encode failed"); }

    TEST("encode/decode HEADERS frame");
    brpc_frame_t h1 = {
        .stream_id = 3,
        .type = BRPC_FRAME_HEADERS,
        .flags = BRPC_FLAG_END_HEADERS,
        .payload_length = 4,
        .payload = (const uint8_t *)"meta"
    };
    encoded = brpc_frame_encode(&h1, buf, sizeof(buf));
    if (encoded == 14) {
        brpc_frame_t h2;
        int decoded = brpc_frame_decode(buf, (size_t)encoded, &h2);
        if (decoded == 14 && h2.stream_id == 3 &&
            h2.type == BRPC_FRAME_HEADERS &&
            h2.flags == BRPC_FLAG_END_HEADERS) {
            PASS();
        } else { FAIL("decode mismatch"); }
    } else { FAIL("encode failed"); }

    TEST("partial frame returns 0");
    brpc_frame_t f3;
    int consumed = brpc_frame_decode(buf, 5, &f3);
    if (consumed == 0) {
        PASS();
    } else { FAIL("should return 0 for partial"); }

    TEST("empty payload frame");
    brpc_frame_t ping = {
        .stream_id = 0,
        .type = BRPC_FRAME_PING,
        .flags = 0,
        .payload_length = 0,
        .payload = NULL
    };
    encoded = brpc_frame_encode(&ping, buf, sizeof(buf));
    if (encoded == 10) {
        brpc_frame_t pong;
        int decoded = brpc_frame_decode(buf, 10, &pong);
        if (decoded == 10 && pong.type == BRPC_FRAME_PING &&
            pong.payload_length == 0 && pong.payload == NULL) {
            PASS();
        } else { FAIL("decode mismatch"); }
    } else { FAIL("encode failed"); }

    TEST("reject unknown frame type");
    brpc_frame_t bad = {
        .stream_id = 0,
        .type = 0xFF,
        .flags = 0,
        .payload_length = 0,
        .payload = NULL
    };
    encoded = brpc_frame_encode(&bad, buf, sizeof(buf));
    if (encoded == -1) {
        PASS();
    } else { FAIL("should reject unknown type"); }

    TEST("reject oversized payload");
    brpc_frame_t big = {
        .stream_id = 0,
        .type = BRPC_FRAME_DATA,
        .flags = 0,
        .payload_length = BRPC_FRAME_MAX_PAYLOAD_SIZE + 1,
        .payload = NULL
    };
    encoded = brpc_frame_encode(&big, buf, sizeof(buf));
    if (encoded == -1) {
        PASS();
    } else { FAIL("should reject oversized"); }

    TEST("buffer too small");
    brpc_frame_t small_buf = {
        .stream_id = 0,
        .type = BRPC_FRAME_DATA,
        .flags = 0,
        .payload_length = 5,
        .payload = (const uint8_t *)"hello"
    };
    encoded = brpc_frame_encode(&small_buf, buf, 5);
    if (encoded == -1) {
        PASS();
    } else { FAIL("should reject small buffer"); }
}

/* ========================================================================
 * Stream Tests
 * ======================================================================== */

static void test_stream_basic(void) {
    TEST("stream init and destroy");
    brpc_stream_t s;
    if (brpc_stream_init(&s, 1, 1024) == 0 &&
        s.stream_id == 1 &&
        s.state == BRPC_STREAM_OPEN) {
        brpc_stream_destroy(&s);
        PASS();
    } else { FAIL("init failed"); }

    TEST("stream write and read");
    brpc_stream_init(&s, 1, 256);
    const char *msg = "hello world";
    int written = brpc_stream_write(&s, (const uint8_t *)msg, strlen(msg));
    if (written == (int)strlen(msg)) {
        int avail = brpc_stream_available_write(&s);
        if (avail == 256 - (int)strlen(msg)) {
            brpc_stream_destroy(&s);
            PASS();
        } else { FAIL("available write mismatch"); brpc_stream_destroy(&s); }
    } else { FAIL("write failed"); brpc_stream_destroy(&s); }

    TEST("stream ring buffer wrap-around");
    brpc_stream_init(&s, 1, 16);
    /* Write and read repeatedly to force wrapping on send side */
    char wbuf[8] = "1234567";
    for (int i = 0; i < 20; i++) {
        brpc_stream_write(&s, (const uint8_t *)wbuf, 7);
    }
    if (s.send_head > 0 && s.send_buf != NULL) {
        brpc_stream_destroy(&s);
        PASS();
    } else { FAIL("head/tail mismatch"); brpc_stream_destroy(&s); }

    TEST("stream available read/write");
    brpc_stream_init(&s, 1, 256);
    brpc_stream_write(&s, (const uint8_t *)"abc", 3);
    if (brpc_stream_available_write(&s) == 256 - 3) {
        brpc_stream_destroy(&s);
        PASS();
    } else { FAIL("available mismatch"); brpc_stream_destroy(&s); }

    TEST("stream close transitions");
    brpc_stream_init(&s, 1, 256);
    brpc_stream_close(&s);
    if (s.state == BRPC_STREAM_HALF_CLOSED_LOCAL) {
        /* Simulate remote close */
        s.state = BRPC_STREAM_HALF_CLOSED_REMOTE;
        brpc_stream_close(&s);
        if (s.state == BRPC_STREAM_CLOSED) {
            brpc_stream_destroy(&s);
            PASS();
        } else { FAIL("second close failed"); brpc_stream_destroy(&s); }
    } else { FAIL("first close failed"); brpc_stream_destroy(&s); }

    TEST("stream read in HALF_CLOSED_REMOTE");
    brpc_stream_init(&s, 1, 256);
    s.state = BRPC_STREAM_HALF_CLOSED_REMOTE;
    /* Write to recv buffer directly to simulate received data */
    const char *recv_data = "data";
    const size_t mask = s.recv_buf_size - 1;
    size_t idx = s.recv_head & mask;
    memcpy(s.recv_buf + idx, recv_data, 4);
    s.recv_head += 4;
    char rbuf2[16];
    int n = brpc_stream_read(&s, (uint8_t *)rbuf2, sizeof(rbuf2));
    if (n == 4 && memcmp(rbuf2, "data", 4) == 0) {
        brpc_stream_destroy(&s);
        PASS();
    } else { FAIL("read failed"); brpc_stream_destroy(&s); }

    TEST("stream write rejected when closed");
    brpc_stream_init(&s, 1, 256);
    s.state = BRPC_STREAM_CLOSED;
    if (brpc_stream_write(&s, (const uint8_t *)"x", 1) == -1) {
        brpc_stream_destroy(&s);
        PASS();
    } else { FAIL("should reject write"); brpc_stream_destroy(&s); }
}

/* ========================================================================
 * Channel Tests (loopback socket pair)
 * ======================================================================== */

static void test_channel_basic(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL("socketpair failed");
        return;
    }

    TEST("channel init and destroy");
    brpc_channel_t ch;
    if (brpc_channel_init(&ch, sv[0], 0, 0) == 0 &&
        ch.fd == sv[0] &&
        ch.is_server == 0 &&
        ch.next_stream_id == 1) {
        brpc_channel_destroy(&ch);
        PASS();
    } else { FAIL("init failed"); }

    TEST("open stream");
    brpc_channel_init(&ch, sv[0], 0, 0);
    brpc_stream_t *s = brpc_channel_open_stream(&ch);
    if (s && s->stream_id == 1 && ch.stream_count == 1) {
        PASS();
    } else { FAIL("open stream failed"); }

    TEST("find stream");
    brpc_stream_t *found = brpc_channel_find_stream(&ch, 1);
    if (found == s) {
        PASS();
    } else { FAIL("find stream failed"); }

    TEST("find non-existent stream");
    found = brpc_channel_find_stream(&ch, 999);
    if (found == NULL) {
        PASS();
    } else { FAIL("should return NULL"); }

    TEST("send ping");
    if (brpc_channel_send_ping(&ch) == 0) {
        PASS();
    } else { FAIL("send ping failed"); }

    TEST("close channel");
    if (brpc_channel_close(&ch) == 0 && ch.closed == 1) {
        PASS();
    } else { FAIL("close failed"); }

    brpc_channel_destroy(&ch);
    close(sv[0]);
    close(sv[1]);
}

static void test_channel_send_data(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL("socketpair failed");
        return;
    }

    TEST("send data frame over channel");
    brpc_channel_t ch;
    brpc_channel_init(&ch, sv[0], 0, 0);
    brpc_stream_t *s = brpc_channel_open_stream(&ch);

    const char *payload = "test payload";
    if (brpc_channel_send_data(&ch, s->stream_id,
                               (const uint8_t *)payload,
                               strlen(payload), 0) == 0) {
        PASS();
    } else { FAIL("send data failed"); }

    TEST("send data with end_stream");
    if (brpc_channel_send_data(&ch, s->stream_id,
                               (const uint8_t *)"bye", 3, 1) == 0) {
        PASS();
    } else { FAIL("send end_stream failed"); }

    TEST("send data on closed channel fails");
    brpc_channel_close(&ch);
    if (brpc_channel_send_data(&ch, s->stream_id,
                               (const uint8_t *)"x", 1, 0) == -1) {
        PASS();
    } else { FAIL("should fail on closed channel"); }

    TEST("send data on non-existent stream fails");
    if (brpc_channel_send_data(&ch, 999, (const uint8_t *)"x", 1, 0) == -1) {
        PASS();
    } else { FAIL("should fail on non-existent stream"); }

    /* --- Event-loop integration tests --- */

    TEST("channel_fd returns socket fd");
    brpc_channel_t ch2;
    brpc_channel_init(&ch2, sv[1], 1, 0);
    if (brpc_channel_fd(&ch2) == sv[1]) {
        PASS();
    } else { FAIL("fd mismatch"); }

    TEST("channel_fd returns -1 for NULL");
    if (brpc_channel_fd(NULL) == -1) {
        PASS();
    } else { FAIL("should return -1"); }

    TEST("wants_read true when buffer has space");
    if (brpc_channel_wants_read(&ch2)) {
        PASS();
    } else { FAIL("should want read on fresh channel"); }

    TEST("wants_read false when closed");
    brpc_channel_close(&ch2);
    if (!brpc_channel_wants_read(&ch2)) {
        PASS();
    } else { FAIL("should not want read when closed"); }

    TEST("wants_write returns 0 (no send buffering)");
    brpc_channel_init(&ch2, sv[1], 1, 0);
    if (!brpc_channel_wants_write(&ch2)) {
        PASS();
    } else { FAIL("should return 0"); }

    TEST("wants_read false for NULL channel");
    if (!brpc_channel_wants_read(NULL)) {
        PASS();
    } else { FAIL("should return 0 for NULL"); }

    brpc_channel_destroy(&ch2);
    brpc_channel_destroy(&ch);
    close(sv[0]);
    close(sv[1]);
}

/* ========================================================================
 * Integration: JSON over Channel
 * ======================================================================== */

static int server_got_stream = 0;

static void on_new_stream_cb(brpc_channel_t *ch, brpc_stream_t *s, void *ctx) {
    (void)ch; (void)s; (void)ctx;
    server_got_stream = 1;
}

static void test_json_over_channel(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        FAIL("socketpair failed");
        return;
    }

    TEST("JSON serialization over bRPC channel");
    /* Client side */
    brpc_channel_t client;
    brpc_channel_init(&client, sv[0], 0, 0);
    brpc_stream_t *cs = brpc_channel_open_stream(&client);

    /* Build a JSON RPC request */
    char json_buf[512];
    json_writer_t w;
    json_writer_init(&w, json_buf, sizeof(json_buf));
    json_write_obj_start(&w);
    json_write_obj_key(&w, "method", 6);
    json_write_str(&w, "getUser", 7);
    json_write_obj_key(&w, "id", 2);
    json_write_int(&w, 1);
    json_write_obj_key(&w, "params", 6);
    json_write_obj_start(&w);
    json_write_obj_key(&w, "userId", 6);
    json_write_int(&w, 42);
    json_write_obj_end(&w);
    json_write_obj_end(&w);
    size_t json_len = json_writer_finish(&w);

    /* Send over channel */
    int rc = brpc_channel_send_data(&client, cs->stream_id,
                                    (const uint8_t *)json_buf,
                                    json_len, 1);
    (void)rc;

    /* Server side receives */
    brpc_channel_t server;
    brpc_channel_init(&server, sv[1], 1, 0);

    /* Set up on_new_stream callback */
    static char server_buf[512];
    static size_t server_buf_len = 0;

    server_got_stream = 0;
    server.on_new_stream = on_new_stream_cb;

    /* Read from server */
    if (brpc_channel_recv(&server) == 0 && server_got_stream) {
        /* Parse the received JSON */
        uint8_t arena_buf[4096];
        json_arena_t arena;
        json_arena_init(&arena, arena_buf, sizeof(arena_buf));
        json_parser_t p;
        json_value_t *root;

        /* Get data from the stream */
        brpc_stream_t *ss = &server.streams[0];
        server_buf_len = brpc_stream_available_read(ss);
        brpc_stream_read(ss, (uint8_t *)server_buf, server_buf_len);

        if (json_parse(&p, server_buf, server_buf_len, &arena, &root) == 0) {
            json_value_t *method = json_obj_get(root, "method");
            json_value_t *id = json_obj_get(root, "id");
            json_value_t *params = json_obj_get(root, "params");

            if (method && id && params &&
                json_get_str(method, NULL) &&
                json_get_int(id, 0) == 1) {
                PASS();
            } else { FAIL("JSON structure mismatch"); }
        } else { FAIL("JSON parse failed"); }
    } else { FAIL("recv failed or no stream"); }

    brpc_channel_destroy(&client);
    brpc_channel_destroy(&server);
    close(sv[0]);
    close(sv[1]);
}

/* ========================================================================
 * RPC Tests
 * ======================================================================== */

static int handler_add(const brpc_rpc_request_t *req,
                       brpc_rpc_response_t *resp, void *ctx)
{
    (void)ctx;
    (void)req;
    resp->result = NULL;
    return BRPC_RPC_OK;
}

static int handler_echo(const brpc_rpc_request_t *req,
                        brpc_rpc_response_t *resp, void *ctx)
{
    (void)ctx;
    (void)req;
    resp->result = NULL;
    return BRPC_RPC_OK;
}

static int handler_fail(const brpc_rpc_request_t *req,
                        brpc_rpc_response_t *resp, void *ctx)
{
    (void)req;
    (void)ctx;
    resp->error_code = BRPC_RPC_ERROR_INTERNAL;
    resp->error_message = "Intentional failure";
    return BRPC_RPC_ERROR_INTERNAL;
}

static void dummy_close_cb(brpc_stream_t *s, void *ctx) {
    (void)s;
    *(int *)ctx = 1;
}

static void dummy_disconnect_cb(brpc_channel_t *ch, void *ctx) {
    (void)ch;
    *(int *)ctx = 1;
}

static void test_rpc(void)
{
    TEST("rpc build request");
    char buf[512];
    int len = brpc_rpc_build_request(buf, sizeof(buf), "getUser", "{\"id\":1}", "42");
    if (len > 0 && strstr(buf, "\"method\":\"getUser\"") && strstr(buf, "\"id\":42")) {
        PASS();
    } else { FAIL("build request failed"); }

    TEST("rpc build response");
    len = brpc_rpc_build_response(buf, sizeof(buf), "42", "{\"name\":\"Alice\"}");
    if (len > 0 && strstr(buf, "\"result\":") && strstr(buf, "\"id\":42")) {
        PASS();
    } else { FAIL("build response failed"); }

    TEST("rpc build error");
    len = brpc_rpc_build_error(buf, sizeof(buf), "1", -32601, "Method not found");
    if (len > 0 && strstr(buf, "\"code\":-32601") && strstr(buf, "Method not found")) {
        PASS();
    } else { FAIL("build error failed"); }

    TEST("rpc build batch request");
    brpc_rpc_batch_item_t batch_items[] = {
        { "getUser", "{\"id\":1}", "1" },
        { "notify", "{\"ok\":true}", NULL }
    };
    len = brpc_rpc_build_batch_request(buf, sizeof(buf), batch_items, 2);
    if (len > 0 && buf[0] == '[' &&
        strstr(buf, "\"method\":\"getUser\"") &&
        strstr(buf, "\"method\":\"notify\"") &&
        strstr(buf, "\"id\":1")) {
        PASS();
    } else { FAIL("build batch request failed"); }

    TEST("rpc server init");
    brpc_rpc_server_t srv;
    brpc_rpc_server_init(&srv);
    if (srv.method_count == 0) {
        PASS();
    } else { FAIL("init failed"); }

    TEST("rpc register method");
    brpc_rpc_register(&srv, "add", handler_add, NULL);
    brpc_rpc_register(&srv, "echo", handler_echo, NULL);
    brpc_rpc_register(&srv, "fail", handler_fail, NULL);
    if (srv.method_count == 3) {
        PASS();
    } else { FAIL("register failed"); }

    TEST("rpc dispatch request");
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    brpc_channel_t ch;
    brpc_channel_init(&ch, sv[0], 0, 0);
    brpc_stream_t *cs = brpc_channel_open_stream(&ch);

    const char *req = "{\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"params\":\"hello\",\"id\":1}";
    int rc = brpc_rpc_server_dispatch(&srv, &ch, cs->stream_id,
                                      req, strlen(req));
    if (rc == 0) {
        PASS();
    } else { FAIL("dispatch failed"); }

    TEST("rpc dispatch notification");
    const char *notif = "{\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"params\":\"hi\"}";
    rc = brpc_rpc_server_dispatch(&srv, &ch, cs->stream_id,
                                  notif, strlen(notif));
    if (rc == 0) {
        PASS();
    } else { FAIL("notification failed"); }

    TEST("rpc dispatch invalid json");
    const char *bad = "not json";
    rc = brpc_rpc_server_dispatch(&srv, &ch, cs->stream_id,
                                  bad, strlen(bad));
    if (rc == 0) {
        PASS();
    } else { FAIL("bad json should return 0"); }

    TEST("rpc dispatch unknown method");
    const char *unknown = "{\"jsonrpc\":\"2.0\",\"method\":\"nope\",\"id\":1}";
    rc = brpc_rpc_server_dispatch(&srv, &ch, cs->stream_id,
                                  unknown, strlen(unknown));
    if (rc == 0) {
        PASS();
    } else { FAIL("unknown method should return 0"); }

    TEST("rpc dispatch batch request");
    const char *batch = "["
        "{\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"id\":2},"
        "{\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"params\":\"notify\"},"
        "{\"jsonrpc\":\"2.0\",\"method\":\"nope\",\"id\":3}"
        "]";
    rc = brpc_rpc_server_dispatch(&srv, &ch, cs->stream_id,
                                  batch, strlen(batch));
    if (rc == 0) {
        PASS();
    } else { FAIL("batch dispatch failed"); }

    TEST("rpc client init");
    brpc_rpc_client_t cli;
    brpc_rpc_client_init(&cli, &ch, cs->stream_id);
    if (cli.ch == &ch && cli.stream_id == cs->stream_id) {
        PASS();
    } else { FAIL("client init failed"); }

    TEST("rpc notify");
    rc = brpc_rpc_notify(&cli, "echo", "\"ping\"");
    if (rc == 0) {
        PASS();
    } else { FAIL("notify failed"); }

    TEST("rpc notify batch");
    brpc_rpc_batch_item_t notify_items[] = {
        { "echo", "\"ping\"", "100" },
        { "echo", "\"pong\"", "101" }
    };
    rc = brpc_rpc_notify_batch(&cli, notify_items, 2);
    if (rc == 0) {
        PASS();
    } else { FAIL("notify batch failed"); }

    brpc_channel_destroy(&ch);
    close(sv[0]);
    close(sv[1]);

    /* Test with real socket pair for call/response. */
    TEST("rpc full round-trip");
    int sv2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
    brpc_channel_t srv_ch, cli_ch;
    brpc_channel_init(&srv_ch, sv2[0], 1, 0);
    brpc_channel_init(&cli_ch, sv2[1], 0, 0);

    brpc_rpc_server_t srv2;
    brpc_rpc_server_init(&srv2);
    brpc_rpc_register(&srv2, "ping", handler_echo, NULL);

    brpc_stream_t *cli_s = brpc_channel_open_stream(&cli_ch);
    brpc_rpc_client_t cli2;
    brpc_rpc_client_init(&cli2, &cli_ch, cli_s->stream_id);

    /* Client sends request. */
    const char *ping_req = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":10}";
    brpc_channel_send_data(&cli_ch, cli_s->stream_id,
                           (const uint8_t *)ping_req, strlen(ping_req), 0);

    /* Server receives and dispatches. */
    brpc_channel_recv(&srv_ch);
    /* Find the server-side stream. */
    brpc_stream_t *srv_s = brpc_channel_find_stream(&srv_ch, cli_s->stream_id);
    if (srv_s) {
        char recv_buf[512];
        size_t avail = brpc_stream_available_read(srv_s);
        if (avail > 0 && avail < sizeof(recv_buf)) {
            brpc_stream_read(srv_s, (uint8_t *)recv_buf, avail);
            recv_buf[avail] = '\0';
            rc = brpc_rpc_server_dispatch(&srv2, &srv_ch, srv_s->stream_id,
                                          recv_buf, avail);
            if (rc == 0) {
                PASS();
            } else { FAIL("server dispatch failed"); }
        } else { FAIL("no data on server stream"); }
    } else { FAIL("server stream not found"); }

    brpc_channel_destroy(&srv_ch);
    brpc_channel_destroy(&cli_ch);
    close(sv2[0]);
    close(sv2[1]);

    /* Test RPC call timeout. */
    TEST("rpc call timeout");
    int sv3[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv3);
    brpc_channel_t to_ch;
    brpc_channel_init(&to_ch, sv3[0], 0, 0);
    brpc_stream_t *to_s = brpc_channel_open_stream(&to_ch);
    brpc_rpc_client_t to_cli;
    brpc_rpc_client_init(&to_cli, &to_ch, to_s->stream_id);

    /* Send request but never respond — should timeout. */
    const char *to_req = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":99}";
    brpc_channel_send_data(&to_ch, to_s->stream_id,
                           (const uint8_t *)to_req, strlen(to_req), 0);

    char to_resp[256];
    rc = brpc_rpc_call_timeout(&to_cli, "ping", NULL,
                               to_resp, sizeof(to_resp), 50);
    if (rc == BRPC_RPC_ERROR_TIMEOUT) {
        PASS();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected TIMEOUT (-32001), got %d", rc);
        FAIL(msg);
    }

    brpc_channel_destroy(&to_ch);
    close(sv3[0]);
    close(sv3[1]);

    /* Test stream reset. */
    TEST("channel send rst");
    int sv4[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv4);
    brpc_channel_t rst_ch;
    brpc_channel_init(&rst_ch, sv4[0], 0, 0);
    brpc_stream_t *rst_s = brpc_channel_open_stream(&rst_ch);
    uint32_t rst_sid = rst_s->stream_id;

    rc = brpc_channel_send_rst(&rst_ch, rst_sid, 42);
    if (rc == 0 && rst_s->state == BRPC_STREAM_CLOSED) {
        PASS();
    } else { FAIL("send_rst failed"); }

    TEST("stream reset closes stream");
    brpc_stream_t rst_s2;
    brpc_stream_init(&rst_s2, 5, 1024);
    brpc_stream_reset(&rst_s2, 99);
    if (rst_s2.state == BRPC_STREAM_CLOSED) {
        PASS();
    } else { FAIL("stream_reset should close stream"); }

    TEST("stream send_window returns default");
    brpc_stream_t bw_s;
    brpc_stream_init(&bw_s, 10, 1024);
    if (brpc_stream_send_window(&bw_s) == BRPC_STREAM_DEFAULT_WINDOW) {
        PASS();
    } else { FAIL("send_window should be default"); }

    TEST("stream is_writable on open stream");
    if (brpc_stream_is_writable(&bw_s)) {
        PASS();
    } else { FAIL("open stream should be writable"); }

    TEST("stream is_writable false after close");
    brpc_stream_close(&bw_s);
    if (!brpc_stream_is_writable(&bw_s)) {
        PASS();
    } else { FAIL("closed stream should not be writable"); }

    TEST("stream send_window returns 0 for NULL");
    if (brpc_stream_send_window(NULL) == 0) {
        PASS();
    } else { FAIL("NULL should return 0"); }

    brpc_stream_destroy(&bw_s);

    /* Test lifecycle callbacks. */
    static int close_called = 0;
    static int disconnect_called = 0;

    TEST("on_close callback fires on rst");
    close_called = 0;
    brpc_stream_t cb_s;
    brpc_stream_init(&cb_s, 20, 1024);
    cb_s.on_close = (void (*)(brpc_stream_t *, void *))dummy_close_cb;
    cb_s.user_ctx = &close_called;
    brpc_stream_reset(&cb_s, 0);
    if (close_called) {
        PASS();
    } else { FAIL("on_close not called"); }

    TEST("on_disconnect callback fires");
    disconnect_called = 0;
    int sv5[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv5);
    brpc_channel_t dc_ch;
    brpc_channel_init(&dc_ch, sv5[0], 0, 0);
    dc_ch.on_disconnect = (void (*)(brpc_channel_t *, void *))dummy_disconnect_cb;
    dc_ch.user_ctx = &disconnect_called;
    close(sv5[1]);  /* Close remote end so recv sees EOF. */
    brpc_channel_recv(&dc_ch);
    if (disconnect_called) {
        PASS();
    } else { FAIL("on_disconnect not called"); }
    brpc_channel_destroy(&dc_ch);
    close(sv5[0]);

    /* Test SETTINGS frame. */
    TEST("channel send settings");
    int sv6[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv6);
    brpc_channel_t set_ch;
    brpc_channel_init(&set_ch, sv6[0], 0, 0);
    rc = brpc_channel_send_settings(&set_ch);
    if (rc == 0) {
        PASS();
    } else { FAIL("send_settings failed"); }

    TEST("settings frame round-trips");
    /* Read the SETTINGS frame on the other end. */
    brpc_channel_t set_ch2;
    brpc_channel_init(&set_ch2, sv6[1], 1, 0);
    brpc_channel_recv(&set_ch2);
    /* After recv, protocol_version should be set from the SETTINGS. */
    if (set_ch2.protocol_version == BRPC_PROTOCOL_VERSION) {
        PASS();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected version %d, got %d",
                 BRPC_PROTOCOL_VERSION, set_ch2.protocol_version);
        FAIL(msg);
    }
    brpc_channel_destroy(&set_ch);
    brpc_channel_destroy(&set_ch2);
    close(sv6[0]);
    close(sv6[1]);
}

/* ========================================================================
 * Compression Tests
 * ======================================================================== */

static void test_compression(void)
{
    TEST("compress/decompress round-trip");
    const char *original = "Hello, brpc compression! This is a test payload that should compress well due to repetition. AAAABBBBCCCCDDDD.";
    size_t orig_len = strlen(original);

    size_t max_comp = brpc_compress_max_size(orig_len);
    uint8_t *comp_buf = (uint8_t *)malloc(max_comp);
    uint8_t *decomp_buf = (uint8_t *)malloc(orig_len + 256);

    size_t comp_len = max_comp;
    int rc = brpc_compress_zlib((const uint8_t *)original, orig_len,
                                comp_buf, &comp_len);
    if (rc == 0 && comp_len < orig_len) {
        PASS();
    } else { FAIL("compress failed or no benefit"); }

    TEST("decompress restores original");
    size_t decomp_len = orig_len + 256;
    rc = brpc_decompress_zlib(comp_buf, comp_len, decomp_buf, &decomp_len);
    if (rc == 0 && decomp_len == orig_len &&
        memcmp(decomp_buf, original, orig_len) == 0) {
        PASS();
    } else { FAIL("decompress mismatch"); }

    TEST("channel send with compression");
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    brpc_channel_t ch;
    brpc_channel_init(&ch, sv[0], 0, 0);
    brpc_channel_set_compress(&ch, 1);
    brpc_stream_t *s = brpc_channel_open_stream(&ch);

    rc = brpc_channel_send_data(&ch, s->stream_id,
                                (const uint8_t *)original, orig_len, 0);
    if (rc == 0) {
        PASS();
    } else { FAIL("send with compress failed"); }

    TEST("compressed frame received and decompressed");
    brpc_channel_t srv_ch;
    brpc_channel_init(&srv_ch, sv[1], 1, 0);
    brpc_channel_recv(&srv_ch);

    brpc_stream_t *srv_s = brpc_channel_find_stream(&srv_ch, s->stream_id);
    if (srv_s) {
        size_t avail = brpc_stream_available_read(srv_s);
        if (avail == orig_len) {
            char recv_buf[512];
            brpc_stream_read(srv_s, (uint8_t *)recv_buf, avail);
            if (memcmp(recv_buf, original, orig_len) == 0) {
                PASS();
            } else { FAIL("decompressed data mismatch"); }
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "expected %zu bytes, got %zu", orig_len, avail);
            FAIL(msg);
        }
    } else { FAIL("server stream not found"); }

    free(comp_buf);
    free(decomp_buf);
    brpc_channel_destroy(&ch);
    brpc_channel_destroy(&srv_ch);
    close(sv[0]);
    close(sv[1]);
}

/* ========================================================================
 * TLS Tests
 * ======================================================================== */

static void test_tls(void)
{
    TEST("tls init and shutdown");
    if (brpc_tls_init() == 0) {
        PASS();
    } else { FAIL("tls_init failed"); }

    TEST("tls client context (no verification)");
    brpc_tls_ctx_t *cli_ctx = brpc_tls_ctx_create_client(NULL, NULL);
    if (cli_ctx != NULL) {
        PASS();
    } else { FAIL("client ctx failed"); }

    TEST("tls server context (self-signed)");
    system("openssl req -x509 -newkey rsa:2048 -keyout /tmp/test_key.pem "
           "-out /tmp/test_cert.pem -days 1 -nodes "
           "-subj '/CN=localhost' 2>/dev/null");

    brpc_tls_ctx_t *srv_ctx = brpc_tls_ctx_create_server(
        "/tmp/test_cert.pem", "/tmp/test_key.pem");
    if (srv_ctx != NULL) {
        PASS();
    } else { FAIL("server ctx failed"); }

    TEST("tls error string");
    const char *err = brpc_tls_error_string();
    if (err != NULL) {
        PASS();
    } else { FAIL("error_string returned NULL"); }

    TEST("tls client context NULL CA");
    /* No CA = no verification, should succeed. */
    brpc_tls_ctx_t *ctx2 = brpc_tls_ctx_create_client(NULL, NULL);
    if (ctx2 != NULL) {
        PASS();
    } else { FAIL("NULL CA ctx failed"); }

    TEST("tls connect returns NULL on bad fd");
    brpc_tls_t *tls_bad = brpc_tls_connect(cli_ctx, -1, "localhost");
    if (tls_bad == NULL) {
        PASS();
    } else {
        brpc_tls_free(tls_bad);
        FAIL("should return NULL for bad fd");
    }

    TEST("tls accept returns NULL on bad fd");
    brpc_tls_t *tls_bad2 = brpc_tls_accept(srv_ctx, -1);
    if (tls_bad2 == NULL) {
        PASS();
    } else {
        brpc_tls_free(tls_bad2);
        FAIL("should return NULL for bad fd");
    }

    TEST("tls fd accessor NULL");
    if (brpc_tls_fd(NULL) == -1) {
        PASS();
    } else { FAIL("NULL should return -1"); }

    brpc_tls_ctx_destroy(cli_ctx);
    brpc_tls_ctx_destroy(srv_ctx);
    brpc_tls_ctx_destroy(ctx2);

    remove("/tmp/test_cert.pem");
    remove("/tmp/test_key.pem");
}

/* ========================================================================
 * API Polish Tests
 * ======================================================================== */

static void test_api_polish(void)
{
    TEST("brpc_error_string covers all codes");
    if (strcmp(brpc_error_string(BRPC_OK), "ok") == 0 &&
        strcmp(brpc_error_string(BRPC_ERROR_CLOSED), "channel closed") == 0 &&
        strcmp(brpc_error_string(BRPC_ERROR_TIMEOUT), "timeout") == 0 &&
        strcmp(brpc_error_string(BRPC_ERROR_IO), "I/O error") == 0 &&
        strcmp(brpc_error_string((brpc_error_t)999), "unknown error") == 0) {
        PASS();
    } else { FAIL("error_string mismatch"); }

    TEST("channel reset_ready_iter");
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    brpc_channel_t ch;
    brpc_channel_init(&ch, sv[0], 0, 0);
    brpc_stream_t *s = brpc_channel_open_stream(&ch);
    /* Send data so stream has something to read. */
    brpc_channel_send_data(&ch, s->stream_id,
                           (const uint8_t *)"hi", 2, 0);
    /* Read it on the other end. */
    brpc_channel_t srv_ch;
    brpc_channel_init(&srv_ch, sv[1], 1, 0);
    brpc_channel_recv(&srv_ch);

    /* Iterate ready streams. */
    brpc_stream_t *rs = NULL;
    int count = 0;
    while ((rs = brpc_channel_next_ready_stream(&srv_ch,
                rs ? rs->stream_id : 0)) != NULL) {
        count++;
    }
    if (count > 0) {
        PASS();
    } else { FAIL("no ready streams"); }

    TEST("channel stats");
    brpc_stats_t stats;
    brpc_channel_stats(&ch, &stats);
    if (stats.frames_sent > 0 && stats.streams_opened > 0) {
        PASS();
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "frames_sent=%lu streams_opened=%lu",
                 (unsigned long)stats.frames_sent,
                 (unsigned long)stats.streams_opened);
        FAIL(msg);
    }

    TEST("rpc_server_poll does not crash");
    brpc_rpc_server_t srv;
    brpc_rpc_server_init(&srv);
    /* Send a request so poll has something to recv and dispatch. */
    brpc_rpc_register(&srv, "echo", handler_echo, NULL);
    brpc_channel_send_data(&ch, s->stream_id,
                           (const uint8_t *)"{\"jsonrpc\":\"2.0\",\"method\":\"echo\",\"id\":1}",
                           44, 0);
    int rc = brpc_rpc_server_poll(&srv, &srv_ch);
    if (rc == 0) {
        PASS();
    } else { FAIL("poll returned error"); }

    TEST("channel_init_ex with config");
    brpc_channel_t ch2;
    brpc_channel_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.is_server = 0;
    cfg.max_streams = 16;
    cfg.compress = 1;
    int sv2[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv2);
    rc = brpc_channel_init_ex(&ch2, sv2[0], &cfg);
    if (rc == BRPC_OK && ch2.compress == 1 && ch2.max_streams == 16) {
        PASS();
    } else { FAIL("init_ex failed"); }

    TEST("channel_init_ex NULL config returns error");
    brpc_channel_t ch3;
    rc = brpc_channel_init_ex(&ch3, sv2[0], NULL);
    if (rc == BRPC_ERROR_INVALID_ARGUMENT) {
        PASS();
    } else { FAIL("should return INVALID_ARGUMENT"); }

    brpc_channel_destroy(&ch);
    brpc_channel_destroy(&srv_ch);
    brpc_channel_destroy(&ch2);
    close(sv[0]);
    close(sv[1]);
    close(sv2[0]);
    close(sv2[1]);
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(void) {
    brpc_prof_init();

    printf("=== JSON Hotpath Tests ===\n");
    test_json_primitives();
    test_json_containers();
    test_json_accessors();
    test_json_errors();
    test_json_writer();
    test_json_roundtrip();

    printf("\n=== Frame Tests ===\n");
    test_frame_encode_decode();

    printf("\n=== Stream Tests ===\n");
    test_stream_basic();

    printf("\n=== Channel Tests ===\n");
    test_channel_basic();
    test_channel_send_data();

    printf("\n=== Integration Tests ===\n");
    test_json_over_channel();

    printf("\n=== RPC Tests ===\n");
    test_rpc();

    printf("\n=== Compression Tests ===\n");
    test_compression();

    printf("\n=== API Polish Tests ===\n");
    test_api_polish();

    printf("\n=== TLS Tests ===\n");
    test_tls();

    printf("\n--- Results: %d/%d passed ---\n", tests_passed, tests_run);

    printf("\n--- Profiling Results ---\n");
    brpc_prof_print();

    return tests_passed == tests_run ? 0 : 1;
}

#include "json_hotpath.h"
#include "brpc_frame.h"
#include "brpc_stream.h"
#include "brpc_channel.h"
#include "brpc_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>

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
    brpc_channel_t ch2;
    brpc_channel_init(&ch2, sv[1], 1, 0);
    if (brpc_channel_send_data(&ch2, 999,
                               (const uint8_t *)"x", 1, 0) == -1) {
        PASS();
    } else { FAIL("should fail on bad stream id"); }

    brpc_channel_destroy(&ch);
    brpc_channel_destroy(&ch2);
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

    printf("\n--- Results: %d/%d passed ---\n", tests_passed, tests_run);

    printf("\n--- Profiling Results ---\n");
    brpc_prof_print();

    return tests_passed == tests_run ? 0 : 1;
}

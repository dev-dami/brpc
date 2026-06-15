#include "json_hotpath.h"
#include "brpc_frame.h"
#include "brpc_stream.h"
#include "brpc_channel.h"
#include "brpc_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

static void demo_json_parser(void) {
    printf("--- JSON Parser Demo ---\n");

    uint8_t arena_buf[4096];
    json_arena_t arena;
    json_arena_init(&arena, arena_buf, sizeof(arena_buf));

    const char *json =
        "{"
        "  \"method\": \"getUser\","
        "  \"id\": 1,"
        "  \"params\": {"
        "    \"userId\": 42,"
        "    \"includeProfile\": true"
        "  },"
        "  \"tags\": [\"admin\", \"active\"]"
        "}";

    json_parser_t p;
    json_value_t *root;

    if (json_parse(&p, json, strlen(json), &arena, &root) != 0) {
        fprintf(stderr, "Parse error: %s\n", p.error);
        return;
    }

    printf("Parsed successfully.\n");
    size_t method_len;
    const char *method_str = json_get_str(json_obj_get(root, "method"), &method_len);
    printf("  method: %.*s\n", (int)method_len, method_str);
    printf("  id:     %lld\n", (long long)json_get_int(json_obj_get(root, "id"), 0));

    json_value_t *params = json_obj_get(root, "params");
    if (params) {
        printf("  params.userId: %lld\n",
               (long long)json_get_int(json_obj_get(params, "userId"), 0));
        printf("  params.includeProfile: %s\n",
               json_get_bool(json_obj_get(params, "includeProfile"), 0) ? "true" : "false");
    }

    json_value_t *tags = json_obj_get(root, "tags");
    if (tags) {
        printf("  tags: [");
        for (size_t i = 0; i < json_array_len(tags); i++) {
            size_t len;
            const char *s = json_get_str(json_array_get(tags, i), &len);
            if (i > 0) printf(", ");
            printf("%.*s", (int)len, s);
        }
        printf("]\n");
    }
}

static void demo_json_serializer(void) {
    printf("\n--- JSON Serializer Demo ---\n");

    char buf[1024];
    json_writer_t w;
    json_writer_init(&w, buf, sizeof(buf));

    json_write_obj_start(&w);
    json_write_obj_key(&w, "jsonrpc", 7);
    json_write_str(&w, "2.0", 3);
    json_write_obj_key(&w, "method", 6);
    json_write_str(&w, "add", 3);
    json_write_obj_key(&w, "params", 6);
    json_write_arr_start(&w);
    json_write_int(&w, 1);
    json_write_int(&w, 2);
    json_write_arr_end(&w);
    json_write_obj_end(&w);

    size_t len = json_writer_finish(&w);
    printf("Serialized: %.*s\n", (int)len, buf);
}

static int server_got_stream = 0;

static void on_new_stream_cb(brpc_channel_t *ch, brpc_stream_t *s, void *ctx) {
    (void)ch; (void)ctx;
    server_got_stream = 1;
    printf("Server: new stream %u opened\n", s->stream_id);
}

static void demo_brpc_channel(void) {
    printf("\n--- bRPC Channel Demo ---\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return;
    }

    /* Client */
    brpc_channel_t client;
    brpc_channel_init(&client, sv[0], 0, 0);
    brpc_stream_t *cs = brpc_channel_open_stream(&client);

    /* Build and send a JSON-RPC request */
    char json_buf[512];
    json_writer_t w;
    json_writer_init(&w, json_buf, sizeof(json_buf));
    json_write_obj_start(&w);
    json_write_obj_key(&w, "method", 6);
    json_write_str(&w, "ping", 4);
    json_write_obj_key(&w, "id", 2);
    json_write_int(&w, 1);
    json_write_obj_end(&w);
    size_t json_len = json_writer_finish(&w);

    printf("Client sending JSON-RPC request on stream %u...\n", cs->stream_id);
    brpc_channel_send_data(&client, cs->stream_id,
                           (const uint8_t *)json_buf, json_len, 1);

    /* Server */
    brpc_channel_t server;
    brpc_channel_init(&server, sv[1], 1, 0);

    server_got_stream = 0;
    server.on_new_stream = on_new_stream_cb;

    printf("Server blocking recv...\n");
    if (brpc_channel_recv(&server) == 0 && server_got_stream) {
        brpc_stream_t *ss = &server.streams[0];
        char recv_buf[512];
        size_t recv_len = brpc_stream_available_read(ss);
        brpc_stream_read(ss, (uint8_t *)recv_buf, recv_len);

        printf("Server received %zu bytes: %.*s\n", recv_len, (int)recv_len, recv_buf);

        /* Parse the request */
        uint8_t arena_buf[2048];
        json_arena_t arena;
        json_arena_init(&arena, arena_buf, sizeof(arena_buf));
        json_parser_t p;
        json_value_t *root;

        if (json_parse(&p, recv_buf, recv_len, &arena, &root) == 0) {
            size_t mlen;
            const char *method = json_get_str(json_obj_get(root, "method"), &mlen);
            printf("Parsed RPC method: %.*s\n", (int)mlen, method);

            /* Send response */
            char resp_buf[256];
            json_writer_t rw;
            json_writer_init(&rw, resp_buf, sizeof(resp_buf));
            json_write_obj_start(&rw);
            json_write_obj_key(&rw, "jsonrpc", 7);
            json_write_str(&rw, "2.0", 3);
            json_write_obj_key(&rw, "result", 6);
            json_write_str(&rw, "pong", 4);
            json_write_obj_key(&rw, "id", 2);
            json_write_int(&rw, 1);
            json_write_obj_end(&rw);
            size_t resp_len = json_writer_finish(&rw);

            brpc_channel_send_data(&server, ss->stream_id,
                                   (const uint8_t *)resp_buf, resp_len, 1);
            printf("Server sent response on stream %u\n", ss->stream_id);
        }
    }

    /* Client reads response */
    brpc_channel_recv(&client);
    if (cs) {
        char resp[512];
        size_t resp_len = brpc_stream_available_read(cs);
        if (resp_len > 0) {
            brpc_stream_read(cs, (uint8_t *)resp, resp_len);
            printf("Client received response: %.*s\n", (int)resp_len, resp);
        }
    }

    /* Cleanup */
    brpc_channel_send_goaway(&client, cs ? cs->stream_id : 0, 0);
    brpc_channel_close(&server);

    brpc_channel_destroy(&client);
    brpc_channel_destroy(&server);
    close(sv[0]);
    close(sv[1]);
}

int main(void) {
    printf("bRPC + JSON Hotpath Demo\n");
    printf("========================\n\n");

    demo_json_parser();
    demo_json_serializer();
    demo_brpc_channel();

    printf("\nDone.\n");

    /* Print profiling results */
    brpc_prof_print();

    return 0;
}

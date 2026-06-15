/**
 * brpc_rpc.c — JSON-RPC 2.0 Implementation
 */

#include "brpc_rpc.h"
#include "brpc_channel.h"
#include "json_hotpath.h"
#include "brpc_prof.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>

/* --------------------------------------------------------------------------
 * Server
 * -------------------------------------------------------------------------- */

void brpc_rpc_server_init(brpc_rpc_server_t *srv)
{
    memset(srv, 0, sizeof(*srv));
    json_arena_init(&srv->arena, srv->arena_buf, sizeof(srv->arena_buf));
}

int brpc_rpc_register(brpc_rpc_server_t *srv, const char *method,
                      brpc_rpc_handler_fn handler, void *user_ctx)
{
    if (srv->method_count >= BRPC_RPC_MAX_METHODS) return -1;

    brpc_rpc_method_t *m = &srv->methods[srv->method_count++];
    m->name = method;
    m->name_len = strlen(method);
    m->handler = handler;
    m->user_ctx = user_ctx;
    return 0;
}

void brpc_rpc_set_default(brpc_rpc_server_t *srv,
                          brpc_rpc_handler_fn handler, void *user_ctx)
{
    srv->default_handler = handler;
    srv->default_ctx = user_ctx;
}

static brpc_rpc_method_t *find_method(brpc_rpc_server_t *srv,
                                       const char *name, size_t name_len)
{
    for (int i = 0; i < srv->method_count; i++) {
        if (srv->methods[i].name_len == name_len &&
            memcmp(srv->methods[i].name, name, name_len) == 0) {
            return &srv->methods[i];
        }
    }
    return NULL;
}

int brpc_rpc_server_dispatch(brpc_rpc_server_t *srv,
                             brpc_channel_t *ch, uint32_t stream_id,
                             const char *data, size_t data_len)
{
    uint64_t _t0 = BRPC_PROF_NOW();

    /* Parse the JSON-RPC request. */
    json_arena_reset(&srv->arena);
    json_parser_t p;
    json_value_t *root = NULL;

    if (json_parse(&p, data, data_len, &srv->arena, &root) != 0) {
        /* Parse error — send error response. */
        char resp[256];
        int len = brpc_rpc_build_error(resp, sizeof(resp), "null",
                                       BRPC_RPC_ERROR_PARSE, p.error);
        if (len > 0) {
            brpc_channel_send_data(ch, stream_id, (const uint8_t *)resp,
                                   (size_t)len, 0);
        }
        return 0;
    }

    if (!root || root->type != JSON_OBJECT) {
        char resp[256];
        int len = brpc_rpc_build_error(resp, sizeof(resp), "null",
                                       BRPC_RPC_ERROR_INVALID,
                                       "Request must be a JSON object");
        if (len > 0) {
            brpc_channel_send_data(ch, stream_id, (const uint8_t *)resp,
                                   (size_t)len, 0);
        }
        return 0;
    }

    /* Extract method. */
    json_value_t *method_val = json_obj_get(root, "method");
    if (!method_val || method_val->type != JSON_STRING) {
        char resp[256];
        int len = brpc_rpc_build_error(resp, sizeof(resp), "null",
                                       BRPC_RPC_ERROR_INVALID,
                                       "Missing or invalid 'method' field");
        if (len > 0) {
            brpc_channel_send_data(ch, stream_id, (const uint8_t *)resp,
                                   (size_t)len, 0);
        }
        return 0;
    }

    /* Extract params (optional). */
    json_value_t *params = json_obj_get(root, "params");

    /* Extract id (NULL for notifications). */
    json_value_t *id = json_obj_get(root, "id");
    int is_notification = (id == NULL || id->type == JSON_NULL);

    /* Build request struct. */
    brpc_rpc_request_t req;
    req.method = method_val->str.ptr;
    req.method_len = method_val->str.len;
    req.params = params;
    req.id = id;
    req.is_notification = is_notification;

    /* Find handler. */
    brpc_rpc_method_t *m = find_method(srv, req.method, req.method_len);

    brpc_rpc_handler_fn handler;
    void *handler_ctx;

    if (m) {
        handler = m->handler;
        handler_ctx = m->user_ctx;
    } else if (srv->default_handler) {
        handler = srv->default_handler;
        handler_ctx = srv->default_ctx;
    } else {
        /* Method not found. */
        if (!is_notification) {
            char resp[256];
            char method_name[128];
            size_t copy_len = req.method_len < sizeof(method_name) - 1
                              ? req.method_len : sizeof(method_name) - 1;
            memcpy(method_name, req.method, copy_len);
            method_name[copy_len] = '\0';

            char errmsg[256];
            snprintf(errmsg, sizeof(errmsg), "Method '%s' not found", method_name);

            int len = brpc_rpc_build_error(resp, sizeof(resp), "null",
                                           BRPC_RPC_ERROR_METHOD, errmsg);
            if (len > 0) {
                brpc_channel_send_data(ch, stream_id, (const uint8_t *)resp,
                                       (size_t)len, 0);
            }
        }
        return 0;
    }

    /* Call handler. */
    brpc_rpc_response_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.id = id;

    int rc = handler(&req, &resp, handler_ctx);

    /* Send response (skip for notifications). */
    if (!is_notification) {
        char resp_buf[4096];

        /* Build the id string for the response. */
        char id_str[64];
        if (id && id->type == JSON_INT) {
            snprintf(id_str, sizeof(id_str), "%lld", (long long)id->i);
        } else if (id && id->type == JSON_STRING) {
            size_t copy_len = id->str.len < sizeof(id_str) - 1
                              ? id->str.len : sizeof(id_str) - 1;
            memcpy(id_str, id->str.ptr, copy_len);
            id_str[copy_len] = '\0';
            /* Wrap in quotes for JSON. */
            memmove(id_str + 1, id_str, copy_len + 1);
            id_str[0] = '"';
            id_str[copy_len + 1] = '"';
            id_str[copy_len + 2] = '\0';
        } else {
            strcpy(id_str, "null");
        }

        int len;
        if (rc != 0 || resp.error_code != 0) {
            int err_code = resp.error_code ? resp.error_code : rc;
            const char *err_msg = resp.error_message ? resp.error_message : "Internal error";
            len = brpc_rpc_build_error(resp_buf, sizeof(resp_buf),
                                       id_str, err_code, err_msg);
        } else {
            /* Serialize result to JSON. */
            char result_json[2048];
            size_t result_len = 0;
            if (resp.result) {
                json_serialize(resp.result, result_json, sizeof(result_json), &result_len);
            } else {
                strcpy(result_json, "null");
                result_len = 4;
            }
            len = brpc_rpc_build_response(resp_buf, sizeof(resp_buf),
                                           id_str, result_json);
        }

        if (len > 0) {
            brpc_channel_send_data(ch, stream_id, (const uint8_t *)resp_buf,
                                   (size_t)len, 0);
        }
    }

    BRPC_PROF_RECORD("rpc_dispatch", BRPC_PROF_NOW() - _t0, data_len);
    return 0;
}

int brpc_rpc_server_poll(brpc_rpc_server_t *srv, brpc_channel_t *ch)
{
    if (!srv || !ch) return -1;

    /* Receive pending data. */
    int rc = brpc_channel_recv(ch);
    if (rc != 0) return rc;

    /* Dispatch all ready streams. */
    brpc_stream_t *s = NULL;
    brpc_channel_reset_ready_iter(ch);
    while ((s = brpc_channel_next_ready_stream(ch,
                s ? s->stream_id : 0)) != NULL) {
        char buf[4096];
        size_t avail = brpc_stream_available_read(s);
        if (avail == 0) continue;
        if (avail > sizeof(buf) - 1) avail = sizeof(buf) - 1;

        int n = brpc_stream_read(s, (uint8_t *)buf, avail);
        if (n <= 0) continue;
        buf[n] = '\0';

        brpc_rpc_server_dispatch(srv, ch, s->stream_id, buf, (size_t)n);
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Client
 * -------------------------------------------------------------------------- */

void brpc_rpc_client_init(brpc_rpc_client_t *cli,
                          brpc_channel_t *ch, uint32_t stream_id)
{
    cli->ch = ch;
    cli->stream_id = stream_id;
}

int brpc_rpc_call(brpc_rpc_client_t *cli, const char *method,
                  const char *params, char *resp_buf, size_t buf_len)
{
    /* Build request. */
    char req_buf[4096];
    int req_len = brpc_rpc_build_request(req_buf, sizeof(req_buf),
                                         method, params, "1");
    if (req_len < 0) return -1;

    /* Send request. */
    int rc = brpc_channel_send_data(cli->ch, cli->stream_id,
                                    (const uint8_t *)req_buf,
                                    (size_t)req_len, 0);
    if (rc != 0) return -1;

    /* Receive response. */
    rc = brpc_channel_recv(cli->ch);
    if (rc != 0) return -1;

    /* Read from stream. */
    brpc_stream_t *s = brpc_channel_find_stream(cli->ch, cli->stream_id);
    if (!s) return -1;

    size_t available = brpc_stream_available_read(s);
    if (available == 0 || available > buf_len) return -1;

    int n = brpc_stream_read(s, (uint8_t *)resp_buf, available);
    if (n <= 0) return -1;

    resp_buf[n] = '\0';
    return 0;
}

int brpc_rpc_call_timeout(brpc_rpc_client_t *cli, const char *method,
                          const char *params, char *resp_buf, size_t buf_len,
                          int timeout_ms)
{
    /* Build request. */
    char req_buf[4096];
    int req_len = brpc_rpc_build_request(req_buf, sizeof(req_buf),
                                         method, params, "1");
    if (req_len < 0) return -1;

    /* Send request. */
    int rc = brpc_channel_send_data(cli->ch, cli->stream_id,
                                    (const uint8_t *)req_buf,
                                    (size_t)req_len, 0);
    if (rc != 0) return -1;

    /* Wait for response with timeout using poll(). */
    struct pollfd pfd;
    pfd.fd = cli->ch->fd;
    pfd.events = POLLIN;

    int remaining_ms = timeout_ms;
    uint64_t deadline = 0;

    if (timeout_ms > 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        deadline = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL
                   + (uint64_t)timeout_ms;
    }

    for (;;) {
        int poll_rc = poll(&pfd, 1, remaining_ms);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                /* Recompute remaining time. */
                if (timeout_ms > 0) {
                    struct timeval tv;
                    gettimeofday(&tv, NULL);
                    uint64_t now = (uint64_t)tv.tv_sec * 1000ULL
                                   + (uint64_t)tv.tv_usec / 1000ULL;
                    if (now >= deadline) return BRPC_RPC_ERROR_TIMEOUT;
                    remaining_ms = (int)(deadline - now);
                }
                continue;
            }
            return -1;
        }
        if (poll_rc == 0) {
            /* Timeout. */
            return BRPC_RPC_ERROR_TIMEOUT;
        }

        /* Data available — try to recv. */
        rc = brpc_channel_recv(cli->ch);
        if (rc != 0) return -1;

        /* Check if our stream has data. */
        brpc_stream_t *s = brpc_channel_find_stream(cli->ch, cli->stream_id);
        if (!s) return -1;

        size_t available = brpc_stream_available_read(s);
        if (available == 0) {
            /* Got data but not for our stream — keep waiting. */
            if (timeout_ms > 0) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                uint64_t now = (uint64_t)tv.tv_sec * 1000ULL
                               + (uint64_t)tv.tv_usec / 1000ULL;
                if (now >= deadline) return BRPC_RPC_ERROR_TIMEOUT;
                remaining_ms = (int)(deadline - now);
            }
            continue;
        }

        if (available > buf_len) return -1;

        int n = brpc_stream_read(s, (uint8_t *)resp_buf, available);
        if (n <= 0) return -1;

        resp_buf[n] = '\0';
        return 0;
    }
}

int brpc_rpc_call_json(brpc_rpc_client_t *cli, const char *method,
                       json_value_t *params, json_arena_t *resp_arena,
                       json_value_t **result)
{
    /* Serialize params to JSON string. */
    char params_buf[4096];
    if (params) {
        size_t params_len = 0;
        if (json_serialize(params, params_buf, sizeof(params_buf), &params_len) != 0) {
            return -1;
        }
        params_buf[params_len] = '\0';
    }

    /* Build request with numeric ID. */
    char id_str[32];
    static int next_id = 1;
    snprintf(id_str, sizeof(id_str), "%d", next_id++);

    char req_buf[4096];
    int req_len = brpc_rpc_build_request(req_buf, sizeof(req_buf),
                                         method,
                                         params ? params_buf : NULL,
                                         id_str);
    if (req_len < 0) return -1;

    /* Send request. */
    int rc = brpc_channel_send_data(cli->ch, cli->stream_id,
                                    (const uint8_t *)req_buf,
                                    (size_t)req_len, 0);
    if (rc != 0) return -1;

    /* Receive response. */
    rc = brpc_channel_recv(cli->ch);
    if (rc != 0) return -1;

    /* Read from stream. */
    brpc_stream_t *s = brpc_channel_find_stream(cli->ch, cli->stream_id);
    if (!s) return -1;

    size_t available = brpc_stream_available_read(s);
    if (available == 0) return -1;

    char resp_buf[8192];
    if (available > sizeof(resp_buf) - 1) available = sizeof(resp_buf) - 1;

    int n = brpc_stream_read(s, (uint8_t *)resp_buf, available);
    if (n <= 0) return -1;
    resp_buf[n] = '\0';

    /* Parse response. */
    json_parser_t p;
    json_arena_reset(resp_arena);
    if (json_parse(&p, resp_buf, (size_t)n, resp_arena, result) != 0) {
        return BRPC_RPC_ERROR_PARSE;
    }

    /* Check for error in response. */
    json_value_t *err = json_obj_get(*result, "error");
    if (err && err->type == JSON_OBJECT) {
        json_value_t *code = json_obj_get(err, "code");
        if (code) return (int)json_get_int(code, BRPC_RPC_ERROR_INTERNAL);
    }

    return 0;
}

int brpc_rpc_notify(brpc_rpc_client_t *cli, const char *method,
                    const char *params)
{
    char req_buf[4096];
    int req_len = brpc_rpc_build_request(req_buf, sizeof(req_buf),
                                         method, params, NULL);
    if (req_len < 0) return -1;

    return brpc_channel_send_data(cli->ch, cli->stream_id,
                                   (const uint8_t *)req_buf,
                                   (size_t)req_len, 0);
}

int brpc_rpc_cancel(brpc_rpc_client_t *cli)
{
    if (!cli || !cli->ch) return -1;
    return brpc_channel_send_rst(cli->ch, cli->stream_id, 0);
}

/* --------------------------------------------------------------------------
 * JSON-RPC message builders
 * -------------------------------------------------------------------------- */

int brpc_rpc_build_request(char *buf, size_t buf_len,
                           const char *method, const char *params,
                           const char *id)
{
    json_writer_t w;
    json_writer_init(&w, buf, buf_len);

    json_write_obj_start(&w);
    json_write_obj_key(&w, "jsonrpc", 7);
    json_write_str(&w, "2.0", 3);

    json_write_obj_key(&w, "method", 6);
    json_write_str(&w, method, strlen(method));

    if (params) {
        json_write_obj_key(&w, "params", 6);
        /* Write raw JSON params (already serialized). */
        json_write_raw(&w, params, strlen(params));
    }

    if (id) {
        json_write_obj_key(&w, "id", 2);
        json_write_raw(&w, id, strlen(id));
    } else {
        json_write_obj_key(&w, "id", 2);
        json_write_null(&w);
    }

    json_write_obj_end(&w);
    return (int)json_writer_finish(&w);
}

int brpc_rpc_build_response(char *buf, size_t buf_len,
                            const char *id, const char *result)
{
    json_writer_t w;
    json_writer_init(&w, buf, buf_len);

    json_write_obj_start(&w);
    json_write_obj_key(&w, "jsonrpc", 7);
    json_write_str(&w, "2.0", 3);

    json_write_obj_key(&w, "result", 6);
    json_write_raw(&w, result, strlen(result));

    json_write_obj_key(&w, "id", 2);
    json_write_raw(&w, id, strlen(id));

    json_write_obj_end(&w);
    return (int)json_writer_finish(&w);
}

int brpc_rpc_build_error(char *buf, size_t buf_len,
                         const char *id, int code, const char *message)
{
    json_writer_t w;
    json_writer_init(&w, buf, buf_len);

    json_write_obj_start(&w);
    json_write_obj_key(&w, "jsonrpc", 7);
    json_write_str(&w, "2.0", 3);

    json_write_obj_key(&w, "error", 5);
    json_write_obj_start(&w);
    json_write_obj_key(&w, "code", 4);
    json_write_int(&w, code);
    json_write_obj_key(&w, "message", 7);
    json_write_str(&w, message, strlen(message));
    json_write_obj_end(&w);

    json_write_obj_key(&w, "id", 2);
    json_write_raw(&w, id, strlen(id));

    json_write_obj_end(&w);
    return (int)json_writer_finish(&w);
}

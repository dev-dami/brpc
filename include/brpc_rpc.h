/**
 * brpc_rpc.h — JSON-RPC 2.0 Abstraction Layer
 *
 * Provides request/response semantics on top of brpc_channel streams.
 * Compatible with the JSON-RPC 2.0 specification.
 *
 * Usage (server):
 *   brpc_rpc_server_t srv;
 *   brpc_rpc_server_init(&srv);
 *   brpc_rpc_register(&srv, "getUser", handler_get_user, NULL);
 *   // In your recv loop:
 *   brpc_rpc_server_dispatch(&srv, stream, recv_buf, recv_len);
 *
 * Usage (client):
 *   brpc_rpc_client_t cli;
 *   brpc_rpc_client_init(&cli, &channel, stream_id);
 *   brpc_rpc_call(&cli, "getUser", "{\"id\":1}", response_buf, sizeof(response_buf));
 */

#pragma once

#include "json_hotpath.h"
#include "brpc_channel.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Error codes (JSON-RPC 2.0 standard)
 * -------------------------------------------------------------------------- */

#define BRPC_RPC_OK                 0
#define BRPC_RPC_ERROR_PARSE       -32700
#define BRPC_RPC_ERROR_INVALID     -32600
#define BRPC_RPC_ERROR_METHOD      -32601
#define BRPC_RPC_ERROR_PARAMS      -32602
#define BRPC_RPC_ERROR_INTERNAL    -32603
#define BRPC_RPC_ERROR_SERVER      -32000

/* --------------------------------------------------------------------------
 * Request / Response
 * -------------------------------------------------------------------------- */

typedef struct brpc_rpc_request {
    const char *method;         /**< Method name (points into parsed JSON).  */
    size_t      method_len;
    json_value_t *params;       /**< Params value (NULL if no params).       */
    json_value_t *id;           /**< Request ID (NULL for notifications).    */
    int         is_notification;/**< Non-zero if id is NULL (no response).   */
} brpc_rpc_request_t;

typedef struct brpc_rpc_response {
    json_value_t *id;           /**< Request ID from original request.       */
    json_value_t *result;       /**< Result value (NULL on error).           */
    int          error_code;    /**< 0 on success, negative on error.        */
    const char  *error_message; /**< Error message (NULL on success).        */
} brpc_rpc_response_t;

/* --------------------------------------------------------------------------
 * Handler callback
 * -------------------------------------------------------------------------- */

/**
 * Method handler function signature.
 *
 * @param req       The parsed request.
 * @param resp      Output: write result or error here.
 * @param user_ctx  User context passed during registration.
 * @return 0 on success, negative error code on failure.
 */
typedef int (*brpc_rpc_handler_fn)(const brpc_rpc_request_t *req,
                                   brpc_rpc_response_t *resp,
                                   void *user_ctx);

/* --------------------------------------------------------------------------
 * Server — method registry + dispatch
 * -------------------------------------------------------------------------- */

#define BRPC_RPC_MAX_METHODS 64

typedef struct brpc_rpc_method {
    const char *name;
    size_t      name_len;
    brpc_rpc_handler_fn handler;
    void *user_ctx;
} brpc_rpc_method_t;

typedef struct brpc_rpc_server {
    brpc_rpc_method_t methods[BRPC_RPC_MAX_METHODS];
    int method_count;

    /* Default handler for unrecognized methods. */
    brpc_rpc_handler_fn default_handler;
    void *default_ctx;

    /* Arena for parsing requests (reused across dispatches). */
    uint8_t  arena_buf[8192];
    json_arena_t arena;
} brpc_rpc_server_t;

/**
 * Initialize the RPC server.
 */
void brpc_rpc_server_init(brpc_rpc_server_t *srv);

/**
 * Register a method handler.
 *
 * @return 0 on success, -1 if registry is full.
 */
int brpc_rpc_register(brpc_rpc_server_t *srv, const char *method,
                      brpc_rpc_handler_fn handler, void *user_ctx);

/**
 * Set a default handler for unregistered methods.
 */
void brpc_rpc_set_default(brpc_rpc_server_t *srv,
                          brpc_rpc_handler_fn handler, void *user_ctx);

/**
 * Dispatch an incoming JSON-RPC message on a stream.
 *
 * Parses the JSON, finds the method, calls the handler, and sends
 * the response back on the same stream.
 *
 * For notifications (no id), no response is sent.
 *
 * @param stream    The stream that received the message.
 * @param data      Raw JSON-RPC message bytes.
 * @param data_len  Length of data.
 * @return 0 on success, -1 on fatal error.
 */
int brpc_rpc_server_dispatch(brpc_rpc_server_t *srv,
                             brpc_channel_t *ch, uint32_t stream_id,
                             const char *data, size_t data_len);

/* --------------------------------------------------------------------------
 * Client — call remote methods
 * -------------------------------------------------------------------------- */

typedef struct brpc_rpc_client {
    brpc_channel_t *ch;
    uint32_t stream_id;
} brpc_rpc_client_t;

/**
 * Initialize an RPC client on an existing channel/stream.
 */
void brpc_rpc_client_init(brpc_rpc_client_t *cli,
                          brpc_channel_t *ch, uint32_t stream_id);

/**
 * Call a remote method and wait for the response (raw string API).
 *
 * Sends a JSON-RPC request, blocks until the response arrives,
 * and returns the raw JSON response.
 *
 * @param method    Method name.
 * @param params    JSON-encoded params string (or NULL for no params).
 * @param resp_buf  Buffer to receive the raw JSON response.
 * @param buf_len   Size of resp_buf.
 * @return 0 on success (response in resp_buf), negative error code.
 */
int brpc_rpc_call(brpc_rpc_client_t *cli, const char *method,
                  const char *params, char *resp_buf, size_t buf_len);

/**
 * Call a remote method with json_value_t params (ergonomic API).
 *
 * Serializes params to JSON, sends the request, waits for response,
 * and parses the response into a json_value_t tree.
 *
 * The caller must provide an arena for parsing the response.
 * The arena must outlive the returned json_value_t.
 *
 * @param method    Method name.
 * @param params    Parsed params value (or NULL for no params).
 * @param resp_arena Arena for parsing the response.
 * @param result    Output: parsed response value (or NULL on error).
 * @return 0 on success, negative error code.
 */
int brpc_rpc_call_json(brpc_rpc_client_t *cli, const char *method,
                       json_value_t *params, json_arena_t *resp_arena,
                       json_value_t **result);

/**
 * Send a notification (no response expected).
 *
 * @return 0 on success.
 */
int brpc_rpc_notify(brpc_rpc_client_t *cli, const char *method,
                    const char *params);

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/**
 * Build a JSON-RPC request string.
 *
 * @param buf       Output buffer.
 * @param buf_len   Size of output buffer.
 * @param method    Method name.
 * @param params    Params JSON string (or NULL).
 * @param id        Request ID (pass NULL for notification).
 * @return Bytes written, or -1 on error.
 */
int brpc_rpc_build_request(char *buf, size_t buf_len,
                           const char *method, const char *params,
                           const char *id);

/**
 * Build a JSON-RPC success response string.
 *
 * @return Bytes written, or -1 on error.
 */
int brpc_rpc_build_response(char *buf, size_t buf_len,
                            const char *id, const char *result);

/**
 * Build a JSON-RPC error response string.
 *
 * @return Bytes written, or -1 on error.
 */
int brpc_rpc_build_error(char *buf, size_t buf_len,
                         const char *id, int code, const char *message);

#ifdef __cplusplus
}
#endif

/**
 * brpc_channel.h — bRPC Multiplexed Channel
 *
 * The channel is the top-level object that owns a file descriptor (TCP
 * socket) and multiplexes many bidirectional streams over it.
 *
 * Design decisions:
 *   • Client-initiated streams use odd IDs (1, 3, 5, …).
 *   • Server-initiated streams use even IDs (2, 4, 6, …).
 *   • Up to BRPC_MAX_STREAMS may be open concurrently.
 *   • A connection-level receive buffer accumulates raw bytes from the
 *     socket; the pump/recv functions parse frames from this buffer and
 *     dispatch them to the appropriate stream.
 *
 * Thread safety: none.  The caller must serialise access or use the
 * channel from a single event-loop thread.
 */

#pragma once

#include "brpc_frame.h"
#include "brpc_stream.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Tunables
 * -------------------------------------------------------------------------- */

#define BRPC_MAX_STREAMS            256
#define BRPC_DEFAULT_WINDOW_SIZE    65536
#define BRPC_DEFAULT_STREAM_BUF_SIZE 16384

/** Size of the connection-level receive buffer. */
#define BRPC_CONN_BUF_SIZE          (128u * 1024u)

/* --------------------------------------------------------------------------
 * Channel structure
 * -------------------------------------------------------------------------- */

typedef struct brpc_channel {
    int fd;                              /**< Underlying socket descriptor.    */

    /* ---- stream table ---- */
    brpc_stream_t streams[BRPC_MAX_STREAMS];
    int           stream_count;          /**< Number of active streams.        */
    uint32_t      next_stream_id;        /**< Next ID to assign.              */
    int           is_server;             /**< Non-zero ⇒ we use even IDs.     */

    /* ---- connection receive buffer ---- */
    uint8_t      *conn_buf;              /**< Heap-allocated accumulation buf. */
    size_t        conn_buf_size;         /**< Allocated size.                  */
    size_t        conn_buf_len;          /**< Valid bytes currently buffered.  */

    /* ---- negotiated settings ---- */
    uint32_t      max_concurrent_streams;
    uint32_t      initial_window_size;

    /* ---- callbacks ---- */

    /**
     * Called when the remote side opens a new stream.  The channel has
     * already created and initialised the stream object; the callback
     * should attach user state and callbacks to `s`.
     */
    void (*on_new_stream)(struct brpc_channel *ch, brpc_stream_t *s,
                          void *ctx);

    void *user_ctx;                      /**< Forwarded to callbacks.          */

    int  closed;                         /**< Non-zero after GOAWAY sent/recv. */
} brpc_channel_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * Initialise a channel over an already-connected socket `fd`.
 * `is_server` selects the stream-ID parity (server = even, client = odd).
 *
 * @return 0 on success, -1 on allocation failure.
 */
int brpc_channel_init(brpc_channel_t *ch, int fd, int is_server);

/**
 * Tear down the channel.  Destroys all streams and frees the connection
 * buffer.  Does **not** close `fd` — the caller owns the socket.
 */
void brpc_channel_destroy(brpc_channel_t *ch);

/**
 * Open a new locally-initiated stream.
 *
 * @return Pointer to the initialised stream, or NULL if the maximum
 *         number of streams has been reached.
 */
brpc_stream_t *brpc_channel_open_stream(brpc_channel_t *ch);

/**
 * Find an existing stream by its ID.
 *
 * @return Pointer to the stream, or NULL if not found.
 */
brpc_stream_t *brpc_channel_find_stream(brpc_channel_t *ch,
                                        uint32_t stream_id);

/**
 * Encode and write a raw frame to the channel's fd.
 * This is a low-level call; prefer the higher-level helpers below.
 *
 * @return 0 on success, -1 on error.
 */
int brpc_channel_send_frame(brpc_channel_t *ch, const brpc_frame_t *frame);

/**
 * Read available bytes from the socket into the connection buffer, then
 * parse and dispatch all complete frames.  Blocking read.
 *
 * @return 0 on success (including partial reads), -1 on fatal error or
 *         remote close.
 */
int brpc_channel_recv(brpc_channel_t *ch);

/**
 * Non-blocking variant of recv — uses MSG_DONTWAIT so it returns
 * immediately if no data is available on the socket.
 *
 * @return 0 on success, -1 on fatal error or remote close.
 */
int brpc_channel_pump(brpc_channel_t *ch);

/**
 * Send a DATA frame on `stream_id`.  If `end_stream` is non-zero the
 * END_STREAM flag is set and the local side is half-closed.
 *
 * @return 0 on success, -1 on error.
 */
int brpc_channel_send_data(brpc_channel_t *ch, uint32_t stream_id,
                           const uint8_t *data, size_t len, int end_stream);

/**
 * Send a PING frame (keepalive).  Payload is 8 bytes of opaque data
 * (we send the current timestamp in microseconds).
 *
 * @return 0 on success, -1 on error.
 */
int brpc_channel_send_ping(brpc_channel_t *ch);

/**
 * Send a GOAWAY frame advertising `last_stream_id` and `error_code`,
 * then mark the channel as closed.
 *
 * @return 0 on success, -1 on error.
 */
int brpc_channel_send_goaway(brpc_channel_t *ch, uint32_t last_stream_id,
                             uint32_t error_code);

/**
 * Convenience: send GOAWAY(0, 0) — a clean shutdown with no error.
 *
 * @return 0 on success, -1 on error.
 */
int brpc_channel_close(brpc_channel_t *ch);

#ifdef __cplusplus
}
#endif

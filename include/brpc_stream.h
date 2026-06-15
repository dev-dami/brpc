/**
 * brpc_stream.h — bRPC Bidirectional Stream Abstraction
 *
 * Each stream is an independent, full-duplex byte channel identified by a
 * 32-bit stream ID. Streams are multiplexed over a single TCP connection
 * managed by the brpc_channel layer.
 *
 * Internally, each direction (send / recv) uses a power-of-two ring buffer
 * for zero-copy staging of frame payloads. Flow control windows prevent
 * either side from overwhelming the other.
 *
 * Stream lifecycle (mirrors HTTP/2):
 *
 *   IDLE ──▶ OPEN ──▶ HALF_CLOSED_LOCAL  ──▶ CLOSED
 *                  └──▶ HALF_CLOSED_REMOTE ──▶ CLOSED
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Stream states
 * -------------------------------------------------------------------------- */

#define BRPC_STREAM_IDLE                0
#define BRPC_STREAM_OPEN                1
#define BRPC_STREAM_HALF_CLOSED_LOCAL   2
#define BRPC_STREAM_HALF_CLOSED_REMOTE  3
#define BRPC_STREAM_CLOSED              4

/* --------------------------------------------------------------------------
 * Default sizing
 * -------------------------------------------------------------------------- */

/** Default per-direction ring buffer size (must be power of two). */
#define BRPC_STREAM_DEFAULT_BUF_SIZE  16384

/** Default flow-control window (bytes the peer may send before we ACK). */
#define BRPC_STREAM_DEFAULT_WINDOW    65536

/* --------------------------------------------------------------------------
 * Stream structure
 * -------------------------------------------------------------------------- */

typedef struct brpc_stream {
    /* ---- identity & state ---- */
    uint32_t stream_id;
    int      state;           /**< One of BRPC_STREAM_* constants. */

    /* ---- send ring buffer ---- */
    uint8_t *send_buf;
    size_t   send_buf_size;   /**< Always a power of two.          */
    size_t   send_head;       /**< Write position (producer).      */
    size_t   send_tail;       /**< Read position  (consumer).      */

    /* ---- recv ring buffer ---- */
    uint8_t *recv_buf;
    size_t   recv_buf_size;   /**< Always a power of two.          */
    size_t   recv_head;       /**< Write position (producer).      */
    size_t   recv_tail;       /**< Read position  (consumer).      */

    /* ---- flow control ---- */
    int32_t  send_window;     /**< Bytes we're still allowed to send.  */
    int32_t  recv_window;     /**< Bytes the peer is still allowed to send. */

    /* ---- user callbacks ---- */

    /**
     * Called when new data has been deposited into the recv ring buffer.
     * `data` points directly into the ring buffer (may wrap — the channel
     * layer linearises before calling).  `len` is the number of new bytes.
     */
    void (*on_data)(struct brpc_stream *s, const uint8_t *data,
                    size_t len, void *ctx);

    /**
     * Called when the remote side signals END_STREAM.
     */
    void (*on_end)(struct brpc_stream *s, void *ctx);

    /**
     * Called on stream error (RST_STREAM received or local error).
     * `code` is the error code from the RST_STREAM frame.
     */
    void (*on_error)(struct brpc_stream *s, int code, void *ctx);

    /** Opaque pointer forwarded to all callbacks. */
    void *user_ctx;
} brpc_stream_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * Initialise a stream, allocating ring buffers of `buf_size` bytes each.
 * `buf_size` must be a power of two; if it is not, it is rounded up.
 *
 * @return 0 on success, -1 on allocation failure.
 */
int brpc_stream_init(brpc_stream_t *s, uint32_t stream_id, size_t buf_size);

/**
 * Free ring buffers and zero the stream structure.
 * Safe to call on a zero-initialised or already-destroyed stream.
 */
void brpc_stream_destroy(brpc_stream_t *s);

/**
 * Write up to `len` bytes into the send ring buffer.
 *
 * @return Number of bytes actually written (may be less than `len` if the
 *         ring buffer is full), or -1 if the stream is not writable.
 */
int brpc_stream_write(brpc_stream_t *s, const uint8_t *data, size_t len);

/**
 * Read up to `buf_len` bytes from the recv ring buffer into `buf`.
 *
 * @return Number of bytes read, or -1 if the stream is not readable.
 */
int brpc_stream_read(brpc_stream_t *s, uint8_t *buf, size_t buf_len);

/**
 * Half-close the local send side.  The stream transitions from OPEN to
 * HALF_CLOSED_LOCAL, or from HALF_CLOSED_REMOTE to CLOSED.
 */
void brpc_stream_close(brpc_stream_t *s);

/**
 * Return the number of bytes available for reading.
 */
size_t brpc_stream_available_read(const brpc_stream_t *s);

/**
 * Return the number of free bytes available for writing.
 */
size_t brpc_stream_available_write(const brpc_stream_t *s);

#ifdef __cplusplus
}
#endif

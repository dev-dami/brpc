/**
 * brpc_frame.h — bRPC Binary Framing Protocol
 *
 * Defines an HTTP/3-inspired binary framing layer designed for multiplexed
 * bidirectional streaming over TCP. Every message on the wire is wrapped in
 * a frame with a fixed 10-byte header followed by a variable-length payload.
 *
 * Wire format (all fields little-endian):
 *
 *   Offset  Size  Field
 *   ------  ----  -----
 *   0       4     Stream ID
 *   4       1     Frame Type
 *   5       1     Flags
 *   6       4     Payload Length
 *   10      N     Payload (N = Payload Length)
 *
 * Frame types mirror the essential operations needed for an RPC channel:
 * data transfer, header metadata, stream reset, connection settings,
 * keepalive pings, graceful shutdown, and flow control.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Frame header size constant
 * -------------------------------------------------------------------------- */

/** Total size of the fixed frame header in bytes. */
#define BRPC_FRAME_HEADER_SIZE 10

/** Maximum payload size: 16 MiB. Prevents absurd allocations on corrupt data. */
#define BRPC_FRAME_MAX_PAYLOAD_SIZE (16u * 1024u * 1024u)

/* --------------------------------------------------------------------------
 * Frame types
 * -------------------------------------------------------------------------- */

/** Carries the JSON-encoded RPC request or response body. */
#define BRPC_FRAME_DATA          0x00

/** Carries the method name and metadata key-value pairs. */
#define BRPC_FRAME_HEADERS       0x01

/** Aborts a single stream with an error code. */
#define BRPC_FRAME_RST_STREAM    0x02

/** Connection-level settings negotiation. */
#define BRPC_FRAME_SETTINGS      0x03

/** Keepalive ping (must be echoed back). */
#define BRPC_FRAME_PING          0x04

/** Graceful shutdown: no new streams after this. */
#define BRPC_FRAME_GOAWAY        0x05

/** Flow-control window update for a stream or connection. */
#define BRPC_FRAME_WINDOW_UPDATE 0x06

/* --------------------------------------------------------------------------
 * Frame flags (bitfield)
 * -------------------------------------------------------------------------- */

/** Sender will send no more data on this stream. */
#define BRPC_FLAG_END_STREAM     0x01

/** This frame completes the header block. */
#define BRPC_FLAG_END_HEADERS    0x02

/** Payload is compressed (algorithm defined in SETTINGS). */
#define BRPC_FLAG_COMPRESSED     0x04

/* --------------------------------------------------------------------------
 * Frame structure
 * -------------------------------------------------------------------------- */

/**
 * In-memory representation of a single bRPC frame.
 *
 * The `payload` pointer is *not* owned by this struct — it points into the
 * caller's buffer (for decode) or the caller provides their own buffer
 * (for encode). No heap allocation happens inside encode/decode.
 */
typedef struct brpc_frame {
    uint32_t stream_id;       /**< Logical stream this frame belongs to.     */
    uint8_t  type;            /**< One of BRPC_FRAME_* constants.            */
    uint8_t  flags;           /**< Bitwise OR of BRPC_FLAG_* constants.      */
    uint32_t payload_length;  /**< Length of the payload in bytes.            */
    const uint8_t *payload;   /**< Pointer to payload data (not owned).      */
} brpc_frame_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * Returns the fixed frame header size (always 10).
 */
static inline size_t brpc_frame_header_size(void)
{
    return BRPC_FRAME_HEADER_SIZE;
}

/**
 * Encode a frame into a byte buffer.
 *
 * Writes the 10-byte header followed by `frame->payload_length` bytes of
 * payload into `buf`. The caller must ensure `buf` is large enough.
 *
 * @param frame   Frame to encode (payload pointer must be valid if
 *                payload_length > 0).
 * @param buf     Destination buffer.
 * @param buf_len Size of the destination buffer in bytes.
 *
 * @return  Total bytes written (header + payload) on success.
 *          -1 if the buffer is too small or the frame is invalid.
 */
int brpc_frame_encode(const brpc_frame_t *frame, uint8_t *buf, size_t buf_len);

/**
 * Decode a frame from a byte buffer.
 *
 * Parses the header and validates the frame type. On success, `frame_out`
 * is populated and `frame_out->payload` points directly into `buf` (zero-
 * copy). The caller must not free or modify `buf` while using `frame_out`.
 *
 * @param buf       Source buffer containing wire data.
 * @param buf_len   Number of valid bytes in `buf`.
 * @param frame_out Output frame structure.
 *
 * @return  Total bytes consumed (header + payload) on success.
 *          0 if the buffer does not yet contain a complete frame (partial read).
 *          -1 on validation error (bad frame type, payload too large).
 */
int brpc_frame_decode(const uint8_t *buf, size_t buf_len,
                      brpc_frame_t *frame_out);

#ifdef __cplusplus
}
#endif

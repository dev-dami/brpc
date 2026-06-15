/**
 * brpc_frame.c — bRPC Binary Frame Encoder / Decoder
 *
 * Encodes and decodes frames according to the bRPC wire format.
 * All multi-byte integers are stored in little-endian order.
 * We use memcpy rather than pointer casts to avoid undefined behaviour
 * from unaligned access on strict-alignment architectures.
 */

#include "brpc_frame.h"
#include "brpc_prof.h"

#include <string.h>  /* memcpy */
#include <stdio.h>   /* (for debug, not used in release) */

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * Write a 32-bit unsigned integer in little-endian byte order at `dst`.
 */
static inline void write_le32(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val);
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

/**
 * Read a 32-bit unsigned integer in little-endian byte order from `src`.
 */
static inline uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

/**
 * Validate that `type` is a known frame type.
 * Returns 0 on success, -1 on unknown type.
 */
static inline int validate_frame_type(uint8_t type)
{
    switch (type) {
    case BRPC_FRAME_DATA:
    case BRPC_FRAME_HEADERS:
    case BRPC_FRAME_RST_STREAM:
    case BRPC_FRAME_SETTINGS:
    case BRPC_FRAME_PING:
    case BRPC_FRAME_GOAWAY:
    case BRPC_FRAME_WINDOW_UPDATE:
        return 0;
    default:
        return -1;
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int brpc_frame_encode(const brpc_frame_t *frame, uint8_t *buf, size_t buf_len)
{
    uint64_t _t0 = BRPC_PROF_NOW();

    /* Sanity checks. */
    if (!frame || !buf) {
        return -1;
    }

    /* Reject unknown frame types. */
    if (validate_frame_type(frame->type) != 0) {
        return -1;
    }

    /* Reject payloads that exceed the safety limit. */
    if (frame->payload_length > BRPC_FRAME_MAX_PAYLOAD_SIZE) {
        return -1;
    }

    /* If there is a payload, the caller must supply a pointer. */
    if (frame->payload_length > 0 && !frame->payload) {
        return -1;
    }

    /* Total frame size = header + payload. */
    size_t total = (size_t)BRPC_FRAME_HEADER_SIZE + frame->payload_length;
    if (buf_len < total) {
        return -1;  /* Buffer too small. */
    }

    /*
     * Header layout (10 bytes):
     *   [0..3]  stream_id       (LE32)
     *   [4]     type            (uint8)
     *   [5]     flags           (uint8)
     *   [6..9]  payload_length  (LE32)
     */
    write_le32(buf + 0, frame->stream_id);
    buf[4] = frame->type;
    buf[5] = frame->flags;
    write_le32(buf + 6, frame->payload_length);

    /* Copy payload. */
    if (frame->payload_length > 0) {
        memcpy(buf + BRPC_FRAME_HEADER_SIZE, frame->payload,
               frame->payload_length);
    }

    BRPC_PROF_RECORD("frame_encode", BRPC_PROF_NOW() - _t0, total);
    return (int)total;
}

int brpc_frame_decode(const uint8_t *buf, size_t buf_len,
                      brpc_frame_t *frame_out)
{
    uint64_t _t0 = BRPC_PROF_NOW();

    if (!buf || !frame_out) {
        return -1;
    }

    /* Need at least the header to determine payload length. */
    if (buf_len < BRPC_FRAME_HEADER_SIZE) {
        return 0;  /* Not enough data yet — tell caller to read more. */
    }

    /* Parse the header fields. */
    uint32_t stream_id      = read_le32(buf + 0);
    uint8_t  type            = buf[4];
    uint8_t  flags           = buf[5];
    uint32_t payload_length  = read_le32(buf + 6);

    /* Validate frame type. */
    if (validate_frame_type(type) != 0) {
        return -1;
    }

    /* Reject absurdly large payloads. */
    if (payload_length > BRPC_FRAME_MAX_PAYLOAD_SIZE) {
        return -1;
    }

    /* Total bytes needed for the complete frame. */
    size_t total = (size_t)BRPC_FRAME_HEADER_SIZE + payload_length;

    /* Do we have the full frame yet? */
    if (buf_len < total) {
        return 0;  /* Partial frame — need more data. */
    }

    /* Populate the output structure. */
    frame_out->stream_id      = stream_id;
    frame_out->type            = type;
    frame_out->flags           = flags;
    frame_out->payload_length  = payload_length;

    if (payload_length > 0) {
        /* Point directly into the source buffer (zero-copy). */
        frame_out->payload = buf + BRPC_FRAME_HEADER_SIZE;
    } else {
        frame_out->payload = NULL;
    }

    BRPC_PROF_RECORD("frame_decode", BRPC_PROF_NOW() - _t0, total);
    return (int)total;
}

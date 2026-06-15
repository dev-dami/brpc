/**
 * brpc_channel.c — bRPC Multiplexed Channel Implementation
 *
 * The channel sits between the raw socket and the stream layer.  It owns a
 * connection-level receive buffer that accumulates bytes from the socket.
 * On each recv/pump call it:
 *
 *   1. Reads as many bytes as will fit into conn_buf.
 *   2. Loops over conn_buf parsing complete frames.
 *   3. Dispatches each frame to the appropriate stream (or handles
 *      connection-level frames like PING / GOAWAY / SETTINGS).
 *   4. Compacts the buffer so unconsumed bytes slide to the front.
 *
 * Write-path helpers (send_data, send_ping, etc.) construct a frame in a
 * stack-allocated buffer and write it to the fd in a single call.
 */

#include "brpc_channel.h"
#include "brpc_stream.h"
#include "brpc_prof.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

/* --------------------------------------------------------------------------
 * Internal: write all bytes (loop around short writes)
 * -------------------------------------------------------------------------- */

/**
 * Write a 32-bit unsigned integer in little-endian byte order at `dst`.
 */
static inline void write_le32_settings(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val);
    dst[1] = (uint8_t)(val >> 8);
    dst[2] = (uint8_t)(val >> 16);
    dst[3] = (uint8_t)(val >> 24);
}

/**
 * Write a 16-bit unsigned integer in little-endian byte order at `dst`.
 */
static inline void write_le16_settings(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val);
    dst[1] = (uint8_t)(val >> 8);
}

/**
 * Read a 32-bit unsigned integer in little-endian byte order from `src`.
 */
static inline uint32_t read_le32_settings(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8)
         | ((uint32_t)src[2] << 16)
         | ((uint32_t)src[3] << 24);
}

/**
 * Read a 16-bit unsigned integer in little-endian byte order from `src`.
 */
static inline uint16_t read_le16_settings(const uint8_t *src)
{
    return (uint16_t)src[0] | ((uint16_t)src[1] << 8);
}

/**
 * Send a SETTINGS frame advertising our local settings.
 */
int brpc_channel_send_settings(brpc_channel_t *ch)
{
    /*
     * SETTINGS payload: sequence of key-value pairs.
     * Each pair: LE16 key + LE32 value = 6 bytes.
     */
    uint8_t payload[30];  /* 5 pairs × 6 bytes */
    int off = 0;

    /* MAX_STREAMS */
    write_le16_settings(payload + off, BRPC_SETTINGS_MAX_STREAMS);
    write_le32_settings(payload + off + 2, ch->max_streams);
    off += 6;

    /* WINDOW_SIZE */
    write_le16_settings(payload + off, BRPC_SETTINGS_WINDOW_SIZE);
    write_le32_settings(payload + off + 2, ch->initial_window_size);
    off += 6;

    /* PROTOCOL_VERSION */
    write_le16_settings(payload + off, BRPC_SETTINGS_PROTOCOL_VERSION);
    write_le32_settings(payload + off + 2, BRPC_PROTOCOL_VERSION);
    off += 6;

    /* COMPRESSION */
    write_le16_settings(payload + off, BRPC_SETTINGS_COMPRESSION);
    write_le32_settings(payload + off + 2,
                        ch->compress ? BRPC_COMPRESS_ZLIB : BRPC_COMPRESS_NONE);
    off += 6;

    brpc_frame_t frame;
    frame.stream_id     = 0;
    frame.type           = BRPC_FRAME_SETTINGS;
    frame.flags          = 0;
    frame.payload_length = (uint32_t)off;
    frame.payload        = payload;

    return brpc_channel_send_frame(ch, &frame);
}

/**
 * Parse a SETTINGS frame payload and apply remote settings.
 */
static int parse_settings(brpc_channel_t *ch, const uint8_t *payload,
                           uint32_t length)
{
    size_t off = 0;

    while (off + 6 <= length) {
        uint16_t key = read_le16_settings(payload + off);
        uint32_t val = read_le32_settings(payload + off + 2);
        off += 6;

        switch (key) {
        case BRPC_SETTINGS_MAX_STREAMS:
            /* Respect remote's limit if smaller. */
            if (val < ch->max_streams) {
                ch->max_streams = val;
            }
            break;
        case BRPC_SETTINGS_WINDOW_SIZE:
            ch->initial_window_size = val;
            break;
        case BRPC_SETTINGS_PROTOCOL_VERSION:
            ch->protocol_version = val;
            break;
        case BRPC_SETTINGS_COMPRESSION:
            /* Peer advertises supported compression algorithms.
             * Enable zlib if the peer supports it and we have it. */
            if (val & BRPC_COMPRESS_ZLIB) {
                ch->compress = 1;
            }
            break;
        default:
            break;  /* Ignore unknown keys. */
        }
    }

    return 0;
}

/**
 * Write exactly `len` bytes from `buf` to `fd`, retrying on EINTR.
 * Returns 0 on success, -1 on error.
 */
static int write_all(int fd, const uint8_t *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Internal: deposit data into a stream's recv ring buffer
 * -------------------------------------------------------------------------- */

/**
 * Append `len` bytes of `data` into the stream's receive ring buffer.
 * Returns the number of bytes actually written (may be less than `len`
 * if the ring buffer is full).
 */
static size_t stream_recv_push(brpc_stream_t *s, const uint8_t *data,
                               size_t len)
{
    const size_t mask  = s->recv_buf_size - 1;
    size_t       avail = s->recv_buf_size - (s->recv_head - s->recv_tail);

    if (len > avail) len = avail;
    if (len == 0) return 0;

    size_t idx   = s->recv_head & mask;
    size_t first = s->recv_buf_size - idx;

    if (len <= first) {
        memcpy(s->recv_buf + idx, data, len);
    } else {
        memcpy(s->recv_buf + idx, data, first);
        memcpy(s->recv_buf, data + first, len - first);
    }

    s->recv_head += len;
    return len;
}

/* --------------------------------------------------------------------------
 * Internal: find or implicitly create a stream
 * -------------------------------------------------------------------------- */

/**
 * Look up a stream by ID. If it doesn't exist and we're the server,
 * implicitly create it and notify the application.
 *
 * @return Stream pointer, or NULL if not found (client side) or
 *         max streams reached.
 */
static brpc_stream_t *find_or_create_stream(brpc_channel_t *ch,
                                             uint32_t stream_id)
{
    brpc_stream_t *s = brpc_channel_find_stream(ch, stream_id);
    if (s) return s;

    if (!ch->is_server) return NULL;
    if ((uint32_t)ch->stream_count >= ch->max_streams) return NULL;

    s = &ch->streams[ch->stream_count];
    if (brpc_stream_init(s, stream_id, BRPC_DEFAULT_STREAM_BUF_SIZE) != 0) {
        return NULL;
    }
    ch->stream_count++;
    ch->stat_streams_opened++;

    if (stream_id >= ch->next_stream_id) {
        ch->next_stream_id = stream_id + 2;
    }

    if (ch->on_new_stream) {
        ch->on_new_stream(ch, s, ch->user_ctx);
    }

    return s;
}

/* --------------------------------------------------------------------------
 * Internal: frame dispatch
 * -------------------------------------------------------------------------- */

/**
 * Dispatch a single decoded frame to the appropriate handler.
 * Returns 0 on success, -1 on fatal error.
 */
static int dispatch_frame(brpc_channel_t *ch, const brpc_frame_t *frame)
{
    switch (frame->type) {

    /* ------------------------------------------------------------------ */
    case BRPC_FRAME_DATA: {
        brpc_stream_t *s = find_or_create_stream(ch, frame->stream_id);
        if (!s) return -1;

        /* Push payload into stream recv buffer. */
        if (frame->payload_length > 0) {
            const uint8_t *payload = frame->payload;
            uint32_t payload_len = frame->payload_length;

#ifndef BRPC_NO_COMPRESSION
            /* Decompress if COMPRESSED flag is set. */
            uint8_t  decomp_buf[BRPC_FRAME_MAX_PAYLOAD_SIZE > 65536
                                ? 65536 : BRPC_FRAME_MAX_PAYLOAD_SIZE];
            uint8_t *decomp_ptr = decomp_buf;
            int      decomp_heap = 0;

            if (frame->flags & BRPC_FLAG_COMPRESSED) {
                size_t decomp_len = sizeof(decomp_buf);
                if (decomp_len < (size_t)payload_len * 4) {
                    decomp_len = (size_t)payload_len * 4;
                }
                if (decomp_len > sizeof(decomp_buf)) {
                    decomp_ptr = (uint8_t *)malloc(decomp_len);
                    if (!decomp_ptr) return -1;
                    decomp_heap = 1;
                }
                if (brpc_decompress_zlib(payload, payload_len,
                                         decomp_ptr, &decomp_len) == 0) {
                    payload = decomp_ptr;
                    payload_len = (uint32_t)decomp_len;
                }
                /* If decompression fails, use original (will likely fail downstream). */
            }
#endif

            size_t pushed = stream_recv_push(s, payload, payload_len);
            /* Adjust receive window. */
            s->recv_window -= (int32_t)pushed;

#ifndef BRPC_NO_COMPRESSION
            if (decomp_heap) free(decomp_ptr);
#endif

            /* Invoke data callback. */
            if (s->on_data) {
                s->on_data(s, payload, payload_len, s->user_ctx);
            }
        }

        /* Handle END_STREAM flag. */
        if (frame->flags & BRPC_FLAG_END_STREAM) {
            if (s->state == BRPC_STREAM_OPEN) {
                s->state = BRPC_STREAM_HALF_CLOSED_REMOTE;
            } else if (s->state == BRPC_STREAM_HALF_CLOSED_LOCAL) {
                s->state = BRPC_STREAM_CLOSED;
                ch->stat_streams_closed++;
                if (s->on_close) {
                    s->on_close(s, s->user_ctx);
                }
            }
            if (s->on_end) {
                s->on_end(s, s->user_ctx);
            }
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case BRPC_FRAME_HEADERS: {
        brpc_stream_t *s = find_or_create_stream(ch, frame->stream_id);
        if (!s) return -1;

        /*
         * For now we push the raw header payload into the recv buffer
         * so the application can decode it.  A production implementation
         * would parse key-value pairs here.
         */
        if (frame->payload_length > 0) {
            stream_recv_push(s, frame->payload, frame->payload_length);
            if (s->on_data) {
                s->on_data(s, frame->payload, frame->payload_length,
                           s->user_ctx);
            }
        }

        if (frame->flags & BRPC_FLAG_END_STREAM) {
            if (s->state == BRPC_STREAM_OPEN) {
                s->state = BRPC_STREAM_HALF_CLOSED_REMOTE;
            } else if (s->state == BRPC_STREAM_HALF_CLOSED_LOCAL) {
                s->state = BRPC_STREAM_CLOSED;
                ch->stat_streams_closed++;
                if (s->on_close) {
                    s->on_close(s, s->user_ctx);
                }
            }
            if (s->on_end) {
                s->on_end(s, s->user_ctx);
            }
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case BRPC_FRAME_RST_STREAM: {
        brpc_stream_t *s = brpc_channel_find_stream(ch, frame->stream_id);
        if (!s) break;  /* Ignore RST for unknown streams. */

        /* Extract 4-byte error code from payload (if present). */
        int error_code = 0;
        if (frame->payload_length >= 4 && frame->payload) {
            error_code = (int)( (uint32_t)frame->payload[0]
                              | ((uint32_t)frame->payload[1] << 8)
                              | ((uint32_t)frame->payload[2] << 16)
                              | ((uint32_t)frame->payload[3] << 24));
        }

        s->state = BRPC_STREAM_CLOSED;
        ch->stat_streams_closed++;
        if (s->on_error) {
            s->on_error(s, error_code, s->user_ctx);
        }
        if (s->on_close) {
            s->on_close(s, s->user_ctx);
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case BRPC_FRAME_SETTINGS: {
        /* Parse key-value settings from the payload. */
        if (frame->payload_length > 0 && frame->payload) {
            parse_settings(ch, frame->payload, frame->payload_length);
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case BRPC_FRAME_PING: {
        /*
         * Echo the ping back.  The response carries the same payload
         * and stream_id = 0 (connection-level).  We set END_STREAM as
         * a convention so the peer can distinguish request from reply.
         */
        brpc_frame_t pong;
        pong.stream_id      = 0;
        pong.type            = BRPC_FRAME_PING;
        pong.flags           = BRPC_FLAG_END_STREAM;  /* ACK */
        pong.payload_length  = frame->payload_length;
        pong.payload         = frame->payload;

        if (brpc_channel_send_frame(ch, &pong) != 0) {
            return -1;
        }
        break;
    }

    /* ------------------------------------------------------------------ */
    case BRPC_FRAME_GOAWAY: {
        ch->closed = 1;
        /*
         * Payload layout:
         *   [0..3]  last_stream_id  (LE32)
         *   [4..7]  error_code      (LE32)
         *   [8..]   optional debug data
         *
         * We don't use these yet, but we parse them for completeness.
         */
        (void)frame;
        break;
    }

    /* ------------------------------------------------------------------ */
    case BRPC_FRAME_WINDOW_UPDATE: {
        /*
         * Payload is a 4-byte LE32 window increment.
         * If stream_id == 0 it's a connection-level update (ignored here).
         * Otherwise we credit the target stream's send_window.
         */
        if (frame->payload_length < 4 || !frame->payload) break;

        uint32_t increment = (uint32_t)frame->payload[0]
                           | ((uint32_t)frame->payload[1] << 8)
                           | ((uint32_t)frame->payload[2] << 16)
                           | ((uint32_t)frame->payload[3] << 24);

        if (frame->stream_id != 0) {
            brpc_stream_t *s = brpc_channel_find_stream(ch,
                                                        frame->stream_id);
            if (s) {
                s->send_window += (int32_t)increment;
            }
        }
        break;
    }

    default:
        /* Unknown frame type — already rejected by decode, but guard. */
        return -1;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Internal: parse all complete frames from the connection buffer
 * -------------------------------------------------------------------------- */

/**
 * Walk through conn_buf parsing and dispatching frames until we run out
 * of complete frames, then compact the buffer.
 */
static int parse_frames(brpc_channel_t *ch)
{
    size_t offset = 0;

    while (offset < ch->conn_buf_len) {
        brpc_frame_t frame;
        int consumed = brpc_frame_decode(ch->conn_buf + offset,
                                         ch->conn_buf_len - offset,
                                         &frame);

        if (consumed < 0) {
            /* Corrupt frame — fatal. */
            return -1;
        }
        if (consumed == 0) {
            /* Incomplete frame — wait for more data. */
            break;
        }

        /* Dispatch the fully decoded frame. */
        if (dispatch_frame(ch, &frame) != 0) {
            return -1;
        }

        ch->stat_frames_recv++;
        offset += (size_t)consumed;
    }

    /*
     * Compact: slide unconsumed bytes to the front of the buffer.
     * If offset == conn_buf_len the buffer is now empty.
     */
    if (offset > 0) {
        size_t remaining = ch->conn_buf_len - offset;
        if (remaining > 0) {
            memmove(ch->conn_buf, ch->conn_buf + offset, remaining);
        }
        ch->conn_buf_len = remaining;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int brpc_channel_init(brpc_channel_t *ch, int fd, int is_server,
                      uint32_t max_streams)
{
    if (!ch) return -1;

    memset(ch, 0, sizeof(*ch));

    ch->fd       = fd;
    ch->is_server = is_server;

    /*
     * Client starts stream IDs at 1 (odd).
     * Server starts stream IDs at 2 (even).
     */
    ch->next_stream_id = is_server ? 2 : 1;
    ch->compress       = 0;

    /* Allocate stream table. */
    if (max_streams == 0) max_streams = BRPC_MAX_STREAMS;
    ch->max_streams = max_streams;
    ch->streams = (brpc_stream_t *)calloc(max_streams, sizeof(brpc_stream_t));
    if (!ch->streams) return -1;

    ch->conn_buf = (uint8_t *)malloc(BRPC_CONN_BUF_SIZE);
    if (!ch->streams) { free(ch->streams); return -1; }

    ch->conn_buf_size = BRPC_CONN_BUF_SIZE;
    ch->conn_buf_len  = 0;

    ch->initial_window_size    = BRPC_DEFAULT_WINDOW_SIZE;
    ch->protocol_version       = BRPC_PROTOCOL_VERSION;
    ch->stream_count           = 0;
    ch->closed                 = 0;

    ch->on_new_stream = NULL;
    ch->on_disconnect = NULL;
    ch->user_ctx      = NULL;

    return 0;
}

void brpc_channel_destroy(brpc_channel_t *ch)
{
    if (!ch) return;

    /* Destroy every active stream. */
    for (int i = 0; i < ch->stream_count; i++) {
        brpc_stream_destroy(&ch->streams[i]);
    }

    free(ch->streams);
    free(ch->conn_buf);
    memset(ch, 0, sizeof(*ch));
    ch->fd = -1;  /* Sentinel — we do NOT close the fd. */
}

brpc_stream_t *brpc_channel_open_stream(brpc_channel_t *ch)
{
    if (!ch || ch->closed) return NULL;
    if ((uint32_t)ch->stream_count >= ch->max_streams) return NULL;

    brpc_stream_t *s = &ch->streams[ch->stream_count];

    if (brpc_stream_init(s, ch->next_stream_id,
                         BRPC_DEFAULT_STREAM_BUF_SIZE) != 0) {
        return NULL;
    }

    ch->stream_count++;
    ch->next_stream_id += 2;  /* Maintain odd/even parity. */
    ch->stat_streams_opened++;

    return s;
}

brpc_stream_t *brpc_channel_find_stream(brpc_channel_t *ch,
                                        uint32_t stream_id)
{
    if (!ch) return NULL;

    for (int i = 0; i < ch->stream_count; i++) {
        if (ch->streams[i].stream_id == stream_id) {
            return &ch->streams[i];
        }
    }
    return NULL;
}

int brpc_channel_stream_count(const brpc_channel_t *ch)
{
    return ch ? ch->stream_count : 0;
}

brpc_stream_t *brpc_channel_get_stream(const brpc_channel_t *ch, int index)
{
    if (!ch || index < 0 || index >= ch->stream_count) return NULL;
    return (brpc_stream_t *)&ch->streams[index];
}

brpc_stream_t *brpc_channel_next_ready_stream(const brpc_channel_t *ch,
                                               uint32_t last_id)
{
    if (!ch || !ch->streams) return NULL;

    int start = 0;
    if (last_id != 0) {
        for (int i = 0; i < ch->stream_count; i++) {
            if (ch->streams[i].stream_id == last_id) {
                start = i + 1;
                break;
            }
        }
    }

    for (int i = start; i < ch->stream_count; i++) {
        if (brpc_stream_available_read(&ch->streams[i]) > 0) {
            return (brpc_stream_t *)&ch->streams[i];
        }
    }
    return NULL;
}

int brpc_channel_is_closed(const brpc_channel_t *ch)
{
    return ch ? ch->closed : 1;
}

int brpc_channel_send_frame(brpc_channel_t *ch, const brpc_frame_t *frame)
{
    if (!ch || !frame) return -1;

    /* Encode into a stack buffer.  Max payload is capped at compile time. */
    size_t total = (size_t)BRPC_FRAME_HEADER_SIZE + frame->payload_length;

    /*
     * For large payloads we heap-allocate; for typical RPC messages
     * (< 8 KiB) the stack buffer avoids a malloc.
     */
    uint8_t  stack_buf[8192];
    uint8_t *buf = stack_buf;
    int      heap = 0;

    if (total > sizeof(stack_buf)) {
        buf = (uint8_t *)malloc(total);
        if (!buf) return -1;
        heap = 1;
    }

    int encoded = brpc_frame_encode(frame, buf, total);
    if (encoded < 0) {
        if (heap) free(buf);
        return -1;
    }

    int rc = write_all(ch->fd, buf, (size_t)encoded);
    if (heap) free(buf);

    if (rc == 0) {
        ch->stat_bytes_sent += frame->payload_length;
        ch->stat_frames_sent++;
    }

    return rc;
}

int brpc_channel_recv(brpc_channel_t *ch)
{
    uint64_t _t0 = BRPC_PROF_NOW();

    if (!ch) return -1;

    /* How much room is left in the connection buffer? */
    size_t space = ch->conn_buf_size - ch->conn_buf_len;
    if (space == 0) {
        /*
         * Buffer full but no complete frame could be parsed — the frame
         * is larger than our buffer.  This is a fatal error.
         */
        return -1;
    }

    /* Blocking read. */
    ssize_t n = recv(ch->fd, ch->conn_buf + ch->conn_buf_len, space, 0);

    if (n < 0) {
        if (errno == EINTR) return 0;  /* Interrupted — try again later. */
        return -1;
    }
    if (n == 0) {
        /* Peer closed the connection. */
        ch->closed = 1;
        if (ch->on_disconnect) {
            ch->on_disconnect(ch, ch->user_ctx);
        }
        return -1;
    }

    ch->conn_buf_len += (size_t)n;
    ch->stat_bytes_recv += (size_t)n;

    int rc = parse_frames(ch);
    BRPC_PROF_RECORD("channel_recv", BRPC_PROF_NOW() - _t0, (size_t)n);
    return rc;
}

int brpc_channel_pump(brpc_channel_t *ch)
{
    uint64_t _t0 = BRPC_PROF_NOW();

    if (!ch) return -1;

    size_t space = ch->conn_buf_size - ch->conn_buf_len;
    if (space == 0) return -1;

    /* Non-blocking read. */
    ssize_t n = recv(ch->fd, ch->conn_buf + ch->conn_buf_len, space,
                     MSG_DONTWAIT);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;  /* No data available right now — that's fine. */
        }
        return -1;
    }
    if (n == 0) {
        ch->closed = 1;
        if (ch->on_disconnect) {
            ch->on_disconnect(ch, ch->user_ctx);
        }
        return -1;
    }

    ch->conn_buf_len += (size_t)n;
    ch->stat_bytes_recv += (size_t)n;

    int rc = parse_frames(ch);
    BRPC_PROF_RECORD("channel_pump", BRPC_PROF_NOW() - _t0, (size_t)n);
    return rc;
}

int brpc_channel_send_data(brpc_channel_t *ch, uint32_t stream_id,
                           const uint8_t *data, size_t len, int end_stream)
{
    uint64_t _t0 = BRPC_PROF_NOW();

    if (!ch || ch->closed) return -1;

    brpc_stream_t *s = brpc_channel_find_stream(ch, stream_id);
    if (!s) return -1;

    /* Ensure the stream is in a writable state. */
    if (s->state != BRPC_STREAM_OPEN &&
        s->state != BRPC_STREAM_HALF_CLOSED_REMOTE) {
        return -1;
    }

    brpc_frame_t frame;
    frame.stream_id     = stream_id;
    frame.type           = BRPC_FRAME_DATA;
    frame.flags          = 0;
    frame.payload_length = (uint32_t)len;
    frame.payload        = data;

    if (end_stream) {
        frame.flags |= BRPC_FLAG_END_STREAM;
    }

    /*
     * Compress the payload if compression is enabled.
     * We compress into a stack buffer for small payloads, or heap for large.
     */
#ifndef BRPC_NO_COMPRESSION
    uint8_t  comp_stack[8192];
    uint8_t *comp_buf = comp_stack;
    int      comp_heap = 0;

    if (ch->compress && len > 64) {
        size_t max_comp = brpc_compress_max_size(len);
        if (max_comp > sizeof(comp_stack)) {
            comp_buf = (uint8_t *)malloc(max_comp);
            if (!comp_buf) return -1;
            comp_heap = 1;
        }
        size_t comp_len = max_comp;
        if (brpc_compress_zlib(data, len, comp_buf, &comp_len) == 0) {
            /* Only use compressed version if it's smaller. */
            if (comp_len < len) {
                frame.payload = comp_buf;
                frame.payload_length = (uint32_t)comp_len;
                frame.flags |= BRPC_FLAG_COMPRESSED;
            }
        }
        /* If compression failed or didn't help, use original data. */
    }
#endif

    int rc = brpc_channel_send_frame(ch, &frame);

#ifndef BRPC_NO_COMPRESSION
    if (comp_heap) free(comp_buf);
#endif

    if (rc != 0) return -1;

    /* Debit flow-control window. */
    s->send_window -= (int32_t)len;

    /* Update stream state if we're ending the stream. */
    if (end_stream) {
        brpc_stream_close(s);
    }

    BRPC_PROF_RECORD("channel_send", BRPC_PROF_NOW() - _t0, len);
    return 0;
}

int brpc_channel_send_ping(brpc_channel_t *ch)
{
    if (!ch || ch->closed) return -1;

    /*
     * Ping payload: 8 bytes of opaque data.
     * We fill it with the current time in microseconds so the peer
     * can measure round-trip latency if desired.
     */
    uint8_t payload[8];
    struct timeval tv;
    gettimeofday(&tv, NULL);

    uint64_t usec = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
    memcpy(payload, &usec, sizeof(usec));

    brpc_frame_t frame;
    frame.stream_id     = 0;           /* Connection-level. */
    frame.type           = BRPC_FRAME_PING;
    frame.flags          = 0;
    frame.payload_length = 8;
    frame.payload        = payload;

    return brpc_channel_send_frame(ch, &frame);
}

int brpc_channel_send_goaway(brpc_channel_t *ch, uint32_t last_stream_id,
                             uint32_t error_code)
{
    if (!ch) return -1;

    /*
     * GOAWAY payload:
     *   [0..3]  last_stream_id  (LE32)
     *   [4..7]  error_code      (LE32)
     */
    uint8_t payload[8];
    payload[0] = (uint8_t)(last_stream_id);
    payload[1] = (uint8_t)(last_stream_id >> 8);
    payload[2] = (uint8_t)(last_stream_id >> 16);
    payload[3] = (uint8_t)(last_stream_id >> 24);
    payload[4] = (uint8_t)(error_code);
    payload[5] = (uint8_t)(error_code >> 8);
    payload[6] = (uint8_t)(error_code >> 16);
    payload[7] = (uint8_t)(error_code >> 24);

    brpc_frame_t frame;
    frame.stream_id     = 0;
    frame.type           = BRPC_FRAME_GOAWAY;
    frame.flags          = 0;
    frame.payload_length = 8;
    frame.payload        = payload;

    int rc = brpc_channel_send_frame(ch, &frame);
    if (rc == 0) {
        ch->closed = 1;
    }
    return rc;
}

int brpc_channel_send_rst(brpc_channel_t *ch, uint32_t stream_id,
                          uint32_t error_code)
{
    if (!ch || ch->closed) return -1;

    /*
     * RST_STREAM payload: 4-byte LE32 error code.
     */
    uint8_t payload[4];
    payload[0] = (uint8_t)(error_code);
    payload[1] = (uint8_t)(error_code >> 8);
    payload[2] = (uint8_t)(error_code >> 16);
    payload[3] = (uint8_t)(error_code >> 24);

    brpc_frame_t frame;
    frame.stream_id     = stream_id;
    frame.type           = BRPC_FRAME_RST_STREAM;
    frame.flags          = 0;
    frame.payload_length = 4;
    frame.payload        = payload;

    int rc = brpc_channel_send_frame(ch, &frame);

    /* Also mark the local stream as closed. */
    brpc_stream_t *s = brpc_channel_find_stream(ch, stream_id);
    if (s) {
        s->state = BRPC_STREAM_CLOSED;
        ch->stat_streams_closed++;
        if (s->on_close) {
            s->on_close(s, s->user_ctx);
        }
    }

    return rc;
}

int brpc_channel_close(brpc_channel_t *ch)
{
    /*
     * Clean shutdown: last_stream_id = 0 means "no streams were
     * processed after this point", error_code = 0 means no error.
     */
    return brpc_channel_send_goaway(ch, 0, 0);
}

/* --------------------------------------------------------------------------
 * Event-loop integration
 * -------------------------------------------------------------------------- */

int brpc_channel_fd(const brpc_channel_t *ch)
{
    return ch ? ch->fd : -1;
}

void brpc_channel_set_compress(brpc_channel_t *ch, int enabled)
{
    if (ch) ch->compress = enabled;
}

int brpc_channel_wants_read(const brpc_channel_t *ch)
{
    if (!ch || ch->closed) return 0;
    return ch->conn_buf_len < ch->conn_buf_size;
}

int brpc_channel_wants_write(const brpc_channel_t *ch)
{
    (void)ch;
    /*
     * brpc currently writes directly to the fd in brpc_channel_send_data(),
     * so there is no outgoing buffer to flush.  This returns 0 for now.
     * When send-side buffering is added, this should check whether any
     * stream has pending outgoing data.
     */
    return 0;
}

/* --------------------------------------------------------------------------
 * Stream iterator
 * -------------------------------------------------------------------------- */

void brpc_channel_reset_ready_iter(brpc_channel_t *ch)
{
    if (ch) ch->ready_cursor = 0;
}

/* --------------------------------------------------------------------------
 * Extended init
 * -------------------------------------------------------------------------- */

int brpc_channel_init_ex(brpc_channel_t *ch, int fd,
                         const brpc_channel_config_t *cfg)
{
    if (!ch || !cfg) return BRPC_ERROR_INVALID_ARGUMENT;

    int rc = brpc_channel_init(ch, fd, cfg->is_server, cfg->max_streams);
    if (rc != 0) return BRPC_ERROR_ALLOCATION;

    if (cfg->compress) {
        ch->compress = 1;
    }

    return BRPC_OK;
}

/* --------------------------------------------------------------------------
 * Observability
 * -------------------------------------------------------------------------- */

void brpc_channel_stats(const brpc_channel_t *ch, brpc_stats_t *stats)
{
    if (!ch || !stats) return;

    stats->bytes_sent      = ch->stat_bytes_sent;
    stats->bytes_recv      = ch->stat_bytes_recv;
    stats->frames_sent     = ch->stat_frames_sent;
    stats->frames_recv     = ch->stat_frames_recv;
    stats->streams_opened  = ch->stat_streams_opened;
    stats->streams_closed  = ch->stat_streams_closed;
    stats->rpc_calls       = 0;  /* Filled by RPC layer if needed. */
    stats->rpc_errors      = 0;
}

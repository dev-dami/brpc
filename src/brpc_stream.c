/**
 * brpc_stream.c — bRPC Bidirectional Stream Implementation
 *
 * Ring buffer design:
 *   The buffer size is always a power of two so that index wrapping can be
 *   done with a bitmask (`idx & (size - 1)`) instead of a modulo.  `head`
 *   is the next position to write, `tail` is the next position to read.
 *   The buffer is empty when head == tail and full when
 *   (head - tail) == size.  We allow head and tail to grow without
 *   wrapping the counters themselves — only the actual array index is
 *   masked — which lets us distinguish full from empty without wasting
 *   a slot.
 */

#include "brpc_stream.h"
#include "brpc_prof.h"

#include <stdlib.h>   /* malloc, free, calloc */
#include <string.h>   /* memcpy, memset       */

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * Round `v` up to the next power of two.
 * Returns `v` unchanged if it is already a power of two.
 * Minimum returned value is 1.
 */
static size_t next_pow2(size_t v)
{
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if SIZE_MAX > 0xFFFFFFFFu
    v |= v >> 32;
#endif
    return v + 1;
}

/**
 * Number of bytes stored in a ring buffer.
 * Works correctly even when head has wrapped past SIZE_MAX (unsigned
 * arithmetic gives the correct result).
 */
static inline size_t ring_used(size_t head, size_t tail)
{
    return head - tail;
}

/**
 * Number of free bytes in a ring buffer.
 */
static inline size_t ring_free(size_t head, size_t tail, size_t size)
{
    return size - ring_used(head, tail);
}

/**
 * Masked index into the backing array.
 */
static inline size_t ring_idx(size_t pos, size_t mask)
{
    return pos & mask;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int brpc_stream_init(brpc_stream_t *s, uint32_t stream_id, size_t buf_size)
{
    if (!s) return -1;

    /* Zero everything first so partial-init cleanup is safe. */
    memset(s, 0, sizeof(*s));

    /* Ensure power-of-two buffer size. */
    buf_size = next_pow2(buf_size);
    if (buf_size < 16) buf_size = 16;  /* Sane minimum. */

    s->send_buf = (uint8_t *)malloc(buf_size);
    if (!s->send_buf) {
        return -1;
    }

    s->recv_buf = (uint8_t *)malloc(buf_size);
    if (!s->recv_buf) {
        free(s->send_buf);
        s->send_buf = NULL;
        return -1;
    }

    s->stream_id     = stream_id;
    s->state         = BRPC_STREAM_OPEN;

    s->send_buf_size = buf_size;
    s->send_head     = 0;
    s->send_tail     = 0;

    s->recv_buf_size = buf_size;
    s->recv_head     = 0;
    s->recv_tail     = 0;

    s->send_window   = BRPC_STREAM_DEFAULT_WINDOW;
    s->recv_window   = BRPC_STREAM_DEFAULT_WINDOW;

    s->on_data       = NULL;
    s->on_end        = NULL;
    s->on_error      = NULL;
    s->on_close      = NULL;
    s->user_ctx      = NULL;

    return 0;
}

void brpc_stream_destroy(brpc_stream_t *s)
{
    if (!s) return;

    free(s->send_buf);
    free(s->recv_buf);

    memset(s, 0, sizeof(*s));
    /* state == 0 == BRPC_STREAM_IDLE after memset, which is fine. */
}

int brpc_stream_write(brpc_stream_t *s, const uint8_t *data, size_t len)
{
    uint64_t _t0 = BRPC_PROF_NOW();

    if (!s || !data) return -1;

    /* Only writable in OPEN or HALF_CLOSED_REMOTE states. */
    if (s->state != BRPC_STREAM_OPEN &&
        s->state != BRPC_STREAM_HALF_CLOSED_REMOTE) {
        return -1;
    }

    const size_t mask  = s->send_buf_size - 1;
    size_t       avail = ring_free(s->send_head, s->send_tail,
                                   s->send_buf_size);
    if (len > avail) {
        len = avail;  /* Partial write: as many bytes as we can. */
    }
    if (len == 0) return 0;

    /*
     * Copy data into the ring buffer, handling the wrap-around point.
     * Case 1: the write fits without wrapping.
     * Case 2: we need two memcpy calls — one to the end of the buffer
     *         and one wrapping around to the beginning.
     */
    size_t idx   = ring_idx(s->send_head, mask);
    size_t first = s->send_buf_size - idx;  /* Bytes until wrap. */

    if (len <= first) {
        memcpy(s->send_buf + idx, data, len);
    } else {
        memcpy(s->send_buf + idx, data, first);
        memcpy(s->send_buf, data + first, len - first);
    }

    s->send_head += len;
    BRPC_PROF_RECORD("stream_write", BRPC_PROF_NOW() - _t0, len);
    return (int)len;
}

int brpc_stream_read(brpc_stream_t *s, uint8_t *buf, size_t buf_len)
{
    uint64_t _t0 = BRPC_PROF_NOW();

    if (!s || !buf) return -1;

    /* Only readable in OPEN or HALF_CLOSED_LOCAL states. */
    if (s->state != BRPC_STREAM_OPEN &&
        s->state != BRPC_STREAM_HALF_CLOSED_LOCAL &&
        s->state != BRPC_STREAM_HALF_CLOSED_REMOTE) {
        return -1;
    }

    const size_t mask = s->recv_buf_size - 1;
    size_t       used = ring_used(s->recv_head, s->recv_tail);
    if (buf_len > used) {
        buf_len = used;  /* Only read what's available. */
    }
    if (buf_len == 0) return 0;

    size_t idx   = ring_idx(s->recv_tail, mask);
    size_t first = s->recv_buf_size - idx;

    if (buf_len <= first) {
        memcpy(buf, s->recv_buf + idx, buf_len);
    } else {
        memcpy(buf, s->recv_buf + idx, first);
        memcpy(buf + first, s->recv_buf, buf_len - first);
    }

    s->recv_tail += buf_len;
    BRPC_PROF_RECORD("stream_read", BRPC_PROF_NOW() - _t0, buf_len);
    return (int)buf_len;
}

void brpc_stream_close(brpc_stream_t *s)
{
    if (!s) return;

    switch (s->state) {
    case BRPC_STREAM_OPEN:
        s->state = BRPC_STREAM_HALF_CLOSED_LOCAL;
        break;

    case BRPC_STREAM_HALF_CLOSED_REMOTE:
        s->state = BRPC_STREAM_CLOSED;
        if (s->on_close) {
            s->on_close(s, s->user_ctx);
        }
        break;

    case BRPC_STREAM_HALF_CLOSED_LOCAL:
    case BRPC_STREAM_CLOSED:
    case BRPC_STREAM_IDLE:
    default:
        /* Already closed or idle — nothing to do. */
        break;
    }
}

size_t brpc_stream_available_read(const brpc_stream_t *s)
{
    if (!s) return 0;
    return ring_used(s->recv_head, s->recv_tail);
}

size_t brpc_stream_available_write(const brpc_stream_t *s)
{
    if (!s) return 0;
    return ring_free(s->send_head, s->send_tail, s->send_buf_size);
}

int32_t brpc_stream_send_window(const brpc_stream_t *s)
{
    if (!s) return 0;
    return s->send_window;
}

int brpc_stream_is_writable(const brpc_stream_t *s)
{
    if (!s) return 0;
    if (s->state != BRPC_STREAM_OPEN &&
        s->state != BRPC_STREAM_HALF_CLOSED_REMOTE) {
        return 0;
    }
    if (s->send_window <= 0) return 0;
    return ring_free(s->send_head, s->send_tail, s->send_buf_size) > 0;
}

void brpc_stream_reset(brpc_stream_t *s, int error_code)
{
    if (!s) return;

    s->state = BRPC_STREAM_CLOSED;

    if (s->on_error) {
        s->on_error(s, error_code, s->user_ctx);
    }
    if (s->on_close) {
        s->on_close(s, s->user_ctx);
    }
}

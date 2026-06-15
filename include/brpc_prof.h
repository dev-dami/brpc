#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Profiling configuration
 * -------------------------------------------------------------------------- */

#ifndef BRPC_PROF_ENABLED
#define BRPC_PROF_ENABLED 1
#endif

#ifndef BRPC_PROF_MAX_COUNTERS
#define BRPC_PROF_MAX_COUNTERS 64
#endif

/* --------------------------------------------------------------------------
 * Performance counters
 * -------------------------------------------------------------------------- */

typedef struct brpc_prof_counter {
    const char *name;
    uint64_t    count;       /**< Number of times this counter was hit.      */
    uint64_t    total_ns;    /**< Total time spent in nanoseconds.           */
    uint64_t    min_ns;      /**< Minimum single-call time.                  */
    uint64_t    max_ns;      /**< Maximum single-call time.                  */
    uint64_t    total_bytes; /**< Total bytes processed (for throughput).     */
} brpc_prof_counter_t;

typedef struct brpc_prof {
    brpc_prof_counter_t counters[BRPC_PROF_MAX_COUNTERS];
    int                 count;
    int                 enabled;
} brpc_prof_t;

/* --------------------------------------------------------------------------
 * Global profiler singleton
 * -------------------------------------------------------------------------- */

brpc_prof_t *brpc_prof_global(void);

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/**
 * Initialize the profiler. Called automatically on first use.
 */
void brpc_prof_init(void);

/**
 * Reset all counters to zero.
 */
void brpc_prof_reset(void);

/**
 * Enable or disable profiling.
 */
void brpc_prof_set_enabled(int enabled);

/**
 * Get a counter by name, creating it if it doesn't exist.
 */
brpc_prof_counter_t *brpc_prof_get(const char *name);

/**
 * Record a timed operation.
 */
void brpc_prof_record(const char *name, uint64_t elapsed_ns, uint64_t bytes);

/**
 * Print all counters to stderr.
 */
void brpc_prof_print(void);

/**
 * Print counters in CSV format to a file.
 */
void brpc_prof_print_csv(FILE *f);

/* --------------------------------------------------------------------------
 * Timing helpers
 * -------------------------------------------------------------------------- */

static inline uint64_t brpc_prof_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * Simple timer macro. Usage:
 *   uint64_t _t0 = BRPC_PROF_NOW();
 *   // ... code ...
 *   BRPC_PROF_RECORD("name", BRPC_PROF_NOW() - _t0, bytes);
 */
#define BRPC_PROF_NOW() brpc_prof_now_ns()
#define BRPC_PROF_RECORD(name, ns, bytes) brpc_prof_record(name, ns, bytes)

/* --------------------------------------------------------------------------
 * Per-component counters (convenience macros)
 * -------------------------------------------------------------------------- */

#define BRPC_PROF_JSON_PARSE_START()   BRPC_PROF_SCOPE("json_parse")
#define BRPC_PROF_JSON_SERIAL_START()  BRPC_PROF_SCOPE("json_serialize")
#define BRPC_PROF_FRAME_ENCODE_START() BRPC_PROF_SCOPE("frame_encode")
#define BRPC_PROF_FRAME_DECODE_START() BRPC_PROF_SCOPE("frame_decode")
#define BRPC_PROF_STREAM_WRITE_START() BRPC_PROF_SCOPE("stream_write")
#define BRPC_PROF_STREAM_READ_START()  BRPC_PROF_SCOPE("stream_read")
#define BRPC_PROF_CHANNEL_SEND_START() BRPC_PROF_SCOPE("channel_send")
#define BRPC_PROF_CHANNEL_RECV_START() BRPC_PROF_SCOPE("channel_recv")

#ifdef __cplusplus
}
#endif

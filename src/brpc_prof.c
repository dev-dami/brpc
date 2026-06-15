#include "brpc_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Global profiler
 * -------------------------------------------------------------------------- */

static brpc_prof_t g_prof;
static int g_prof_initialized = 0;

brpc_prof_t *brpc_prof_global(void) {
    if (!g_prof_initialized) {
        brpc_prof_init();
    }
    return &g_prof;
}

void brpc_prof_init(void) {
    if (g_prof_initialized) return;
    memset(&g_prof, 0, sizeof(g_prof));
    g_prof.enabled = 1;
    g_prof_initialized = 1;
}

void brpc_prof_reset(void) {
    brpc_prof_t *p = brpc_prof_global();
    for (int i = 0; i < p->count; i++) {
        memset(&p->counters[i], 0, sizeof(p->counters[i]));
    }
    p->count = 0;
}

void brpc_prof_set_enabled(int enabled) {
    brpc_prof_global()->enabled = enabled;
}

brpc_prof_counter_t *brpc_prof_get(const char *name) {
    brpc_prof_t *p = brpc_prof_global();

    /* Search existing */
    for (int i = 0; i < p->count; i++) {
        if (p->counters[i].name == name ||
            strcmp(p->counters[i].name, name) == 0) {
            return &p->counters[i];
        }
    }

    /* Create new */
    if (p->count >= BRPC_PROF_MAX_COUNTERS) {
        return NULL;
    }

    brpc_prof_counter_t *c = &p->counters[p->count++];
    c->name = name;
    c->count = 0;
    c->total_ns = 0;
    c->min_ns = UINT64_MAX;
    c->max_ns = 0;
    c->total_bytes = 0;
    return c;
}

void brpc_prof_record(const char *name, uint64_t elapsed_ns, uint64_t bytes) {
    brpc_prof_t *p = brpc_prof_global();
    if (!p->enabled) return;

    brpc_prof_counter_t *c = brpc_prof_get(name);
    if (!c) return;

    c->count++;
    c->total_ns += elapsed_ns;
    c->total_bytes += bytes;
    if (elapsed_ns < c->min_ns) c->min_ns = elapsed_ns;
    if (elapsed_ns > c->max_ns) c->max_ns = elapsed_ns;
}

void brpc_prof_print(void) {
    brpc_prof_t *p = brpc_prof_global();

    fprintf(stderr, "\n=== bRPC Profile Counters ===\n");
    fprintf(stderr, "%-25s %8s %12s %12s %12s %12s\n",
            "Counter", "Count", "Total (ms)", "Avg (us)",
            "Min (us)", "Max (us)");
    fprintf(stderr, "%-25s %8s %12s %12s %12s %12s\n",
            "-------------------------", "--------",
            "------------", "------------",
            "------------", "------------");

    for (int i = 0; i < p->count; i++) {
        brpc_prof_counter_t *c = &p->counters[i];
        if (c->count == 0) continue;

        double total_ms = (double)c->total_ns / 1000000.0;
        double avg_us = (double)c->total_ns / (double)c->count / 1000.0;
        double min_us = c->min_ns == UINT64_MAX ? 0 :
                        (double)c->min_ns / 1000.0;
        double max_us = (double)c->max_ns / 1000.0;

        fprintf(stderr, "%-25s %8llu %12.3f %12.3f %12.3f %12.3f\n",
                c->name,
                (unsigned long long)c->count,
                total_ms, avg_us, min_us, max_us);
    }

    fprintf(stderr, "=============================\n\n");
}

void brpc_prof_print_csv(FILE *f) {
    brpc_prof_t *p = brpc_prof_global();

    fprintf(f, "counter,count,total_ns,avg_ns,min_ns,max_ns,total_bytes\n");

    for (int i = 0; i < p->count; i++) {
        brpc_prof_counter_t *c = &p->counters[i];
        if (c->count == 0) continue;

        uint64_t avg_ns = c->total_ns / c->count;

        fprintf(f, "%s,%llu,%llu,%llu,%llu,%llu,%llu\n",
                c->name,
                (unsigned long long)c->count,
                (unsigned long long)c->total_ns,
                (unsigned long long)avg_ns,
                (unsigned long long)(c->min_ns == UINT64_MAX ? 0 : c->min_ns),
                (unsigned long long)c->max_ns,
                (unsigned long long)c->total_bytes);
    }
}

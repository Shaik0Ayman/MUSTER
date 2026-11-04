/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* rig.h - shared scaffolding for the multi-board host tests.
 * Include after muster.h (with MUSTER_IMPLEMENTATION) in exactly one test.
 *
 * Every board is polled once per simulated millisecond. A task can "take
 * time" by calling rig_step(): the other boards keep running while the
 * board executing the task is stuck in it (its own muster_poll() returns
 * at once because it is busy), which is what happens on real hardware.    */
#ifndef RIG_H
#define RIG_H
#include <stdio.h>

static uint32_t g_now;
static uint32_t now_ms(void) { return g_now; }

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  PASS  %s\n", msg); \
                              else { printf("  FAIL  %s\n", msg); fails++; } } while (0)

#define RIG_MAX 4
static muster_t *rig_node[RIG_MAX];
static int       rig_count;
static int       rig_cur = -1;        /* index of the board being polled */

static inline void rig_add(muster_t *m) { rig_node[rig_count++] = m; }

static inline void rig_step(int ms)
{
    int i, k, saved = rig_cur;
    for (k = 0; k < ms; k++) {
        for (i = 0; i < rig_count; i++) { rig_cur = i; muster_poll(rig_node[i]); }
        rig_cur = saved;
        g_now++;
    }
}

/* Poll every board until the call finishes or max_ms passes. */
static inline int8_t rig_wait(muster_t *m, int8_t h, uint8_t *out, uint16_t cap,
                              uint16_t *n, int max_ms)
{
    int8_t st = MUSTER_PENDING;
    int t;
    if (h < 0) return h;
    for (t = 0; t < max_ms; t++) {
        st = muster_result(m, h, out, cap, n);
        if (st != MUSTER_PENDING) return st;
        rig_step(1);
    }
    return st;
}

static inline int rig_done(void)
{
    printf("\n%s\n\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fails ? 1 : 0;
}
#endif

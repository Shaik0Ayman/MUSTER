/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_core.c - core hardening (ROADMAP M1) and regression tests for bugs
 * found in review. Three boards on an 8-byte-MTU bus (classic CAN):
 *   node 0  lead    240 MHz  holds T_NEST, T_BIG
 *   node 1  worker  240 MHz  holds T_COUNT, T_SLOW   (the only holder)
 *   node 2  worker   16 MHz  holds T_SENSOR          (a Nano)
 */
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_loopback.h"
#include "rig.h"

enum { T_COUNT = 1, T_SLOW = 2, T_NEST = 3, T_BIG = 4, T_SENSOR = 5 };

static lb_bus      bus;
static lb_ctx      ctx[3];
static muster_link link_[3];
static muster_t    node[3];

/* ------------------------------------------------------------- tasks */
static int      runs[RIG_MAX];       /* executions per board */
static uint16_t g_slow_ms;
static int8_t   g_nest_rc = 99;

static int16_t tr_count(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_wbuf w; uint32_t sum = 0; uint16_t i;
    if (rig_cur >= 0) runs[rig_cur]++;
    for (i = 0; i < len; i++) sum += a[i];
    muster_w_init(&w, ret, cap); muster_put_u32(&w, sum);
    return w.err ? -1 : (int16_t)w.n;
}

static int16_t tr_slow(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    rig_step(g_slow_ms);                 /* the rest of the system keeps going */
    return tr_count(a, len, ret, cap);
}

static int16_t tr_nest(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    uint8_t out[8]; uint16_t n;
    (void)a; (void)len; (void)ret; (void)cap;
    g_nest_rc = muster_call(&node[0], T_COUNT, 0, 0, out, sizeof out, &n, 10);
    return 0;
}

static int16_t tr_big(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    uint8_t i;
    (void)a; (void)len;
    if (cap < 40) return -1;
    for (i = 0; i < 40; i++) ret[i] = i;
    return 40;
}

/* ---------------------------------------------------- bus fault hook */
typedef struct { uint8_t type, src, dst; int count; } drop_rule;
static drop_rule g_drop;
static uint8_t   g_seen_call, g_seen_status = 0x7F;   /* last RESULT lead -> Nano */

static int hook(lb_bus *b, uint8_t src, const uint8_t *f, uint16_t len)
{
    (void)b;
    if (len < 5 || (f[1] & 0x7Fu) != 0) return 0;     /* first fragment only */
    if (f[2] == M_RESULT && src == 0 && f[0] == 2 && len >= 7) {
        g_seen_call = f[5]; g_seen_status = f[6];
    }
    if (g_drop.count > 0 && f[2] == g_drop.type && src == g_drop.src &&
        (g_drop.dst == 0xFF || f[0] == g_drop.dst)) {
        g_drop.count--;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ events */
static int lost_events, retry_events, joined_events, accepted[3];
static uint8_t last_placed = 0xFF;

static void on_event(uint8_t ev, uint8_t a, uint8_t b, void *user)
{
    int who = (int)(*(const uint8_t *)user);
    (void)a;
    if (who == 0) {
        if (ev == MUSTER_EV_LOST) lost_events++;
        if (ev == MUSTER_EV_RETRY) retry_events++;
        if (ev == MUSTER_EV_JOINED) joined_events++;
        if (ev == MUSTER_EV_PLACED) last_placed = b;
    }
    if (ev == MUSTER_EV_ACCEPTED) accepted[who]++;
}
static const uint8_t ids[3] = {0, 1, 2};

static void start_lead(void)
{
    muster_begin(&node[0], 0, 1, 2, 512, 240, MUSTER_CAP_FPU, &link_[0], now_ms);
    muster_register_task(&node[0], T_NEST, tr_nest, 10);
    muster_register_task(&node[0], T_BIG, tr_big, 10);
    muster_set_cost(&node[0], T_COUNT, 50);           /* held by W1 only */
    muster_on_event(&node[0], on_event, (void *)&ids[0]);
}

static uint32_t sum_of(const uint8_t *out, uint16_t n)
{
    muster_rbuf r; uint32_t v;
    muster_r_init(&r, out, n); v = muster_get_u32(&r);
    return r.err ? 0xFFFFFFFFu : v;
}

int main(void)
{
    const uint8_t args[4] = {1, 2, 3, 4}, args2[4] = {4, 3, 2, 9};
    uint8_t out[64]; uint16_t n = 0; int8_t h, st; int i, t;

    printf("\ncore hardening tests - 3 boards, 8-byte frames\n\n");
    for (i = 0; i < 3; i++) lb_attach(&bus, &ctx[i], &link_[i], (uint8_t)i, 8, 500);
    bus.drop = hook;
    start_lead();
    muster_begin(&node[1], 1, 0, 2, 512, 240, MUSTER_CAP_FPU, &link_[1], now_ms);
    muster_begin(&node[2], 2, 0, 1, 2, 16, 0, &link_[2], now_ms);
    muster_register_task(&node[1], T_COUNT, tr_count, 50);
    muster_register_task(&node[1], T_SLOW, tr_slow, 50);
    muster_register_task(&node[2], T_SENSOR, tr_count, 20);
    for (i = 1; i < 3; i++) muster_on_event(&node[i], on_event, (void *)&ids[i]);
    for (i = 0; i < 3; i++) rig_add(&node[i]);
    rig_step(600);
    CHECK(muster_alive_count(&node[0]) == 3, "setup: three boards joined");

    printf("1. duplicate suppression (M1)\n");
    memset(runs, 0, sizeof runs); retry_events = 0;
    g_drop.type = M_RESULT; g_drop.src = 1; g_drop.dst = 0; g_drop.count = 1;
    h = muster_call_async(&node[0], T_COUNT, args, sizeof args);
    st = rig_wait(&node[0], h, out, sizeof out, &n, 2000);
    CHECK(st == MUSTER_OK && sum_of(out, n) == 10, "call completes after its RESULT was lost");
    CHECK(retry_events >= 1, "lead retried the call");
    CHECK(runs[1] == 1, "a slow worker plus a retry runs the task exactly once");

    memset(runs, 0, sizeof runs);
    bus.ports[1].muted = 1;                         /* W1 hears, can't speak */
    h = muster_call_async(&node[0], T_COUNT, args, sizeof args);
    for (t = 0; t < 450; t++) {
        st = muster_result(&node[0], h, out, sizeof out, &n);
        if (st != MUSTER_PENDING) break;
        rig_step(1);
    }
    bus.ports[1].muted = 0;
    st = rig_wait(&node[0], h, out, sizeof out, &n, 2000);
    CHECK(st == MUSTER_OK && runs[1] == 1,
          "the only holder, silent through a timeout, is retried (not abandoned) and runs once");

    memset(runs, 0, sizeof runs);
    rig_cur = 1;
    m_run_call(&node[1], 0, 0, 200, T_COUNT, args, sizeof args);
    m_run_call(&node[1], 0, 0, 200, T_COUNT, args, sizeof args);
    m_run_call(&node[1], 0, 0, 200, T_COUNT, args2, sizeof args2);
    rig_cur = -1;
    rig_step(5);
    CHECK(runs[1] == 2, "same call ID with different arguments is not mistaken for a duplicate");

    printf("2. calls a worker makes survive loss\n");
    memset(runs, 0, sizeof runs);
    g_drop.type = M_CALL; g_drop.src = 2; g_drop.dst = 0; g_drop.count = 1;
    h = muster_call_async(&node[2], T_COUNT, args, sizeof args);
    st = rig_wait(&node[2], h, out, sizeof out, &n, 3000);
    CHECK(st == MUSTER_OK && sum_of(out, n) == 10, "Nano's CALL was lost; its resend completed");

    memset(runs, 0, sizeof runs);
    g_drop.type = M_RESULT; g_drop.src = 0; g_drop.dst = 2; g_drop.count = 1;
    h = muster_call_async(&node[2], T_COUNT, args, sizeof args);
    st = rig_wait(&node[2], h, out, sizeof out, &n, 3000);
    CHECK(st == MUSTER_OK && sum_of(out, n) == 10, "relayed RESULT was lost; the lead answered the resend");
    CHECK(runs[1] == 1, "...from its cache, without running the task again");

    printf("3. slow tasks\n");
    memset(runs, 0, sizeof runs); lost_events = 0;
    g_slow_ms = 700;                                  /* > call timeout */
    h = muster_call_async(&node[0], T_SLOW, args, sizeof args);
    st = rig_wait(&node[0], h, out, sizeof out, &n, 3000);
    CHECK(st == MUSTER_OK && runs[1] == 1,
          "a 700 ms task on the only holder completes and runs once");
    CHECK(lost_events == 0, "the busy worker was not declared lost");

    memset(runs, 0, sizeof runs);
    g_slow_ms = 300;
    h = muster_call_async(&node[2], T_SLOW, args, sizeof args);
    st = rig_wait(&node[2], h, out, sizeof out, &n, 4000);
    CHECK(st == MUSTER_OK && runs[1] == 1, "a Nano's call to a 300 ms task completes and runs once");

    printf("4. liveness\n");
    {
        int before = accepted[2], live = 1, h2 = -1;
        for (t = 0; t < 3000; t++) {                 /* lead busy talking to W1 */
            if (h2 >= 0 && muster_result(&node[0], (int8_t)h2, out, sizeof out, &n) != MUSTER_PENDING)
                h2 = -1;
            if (h2 < 0) h2 = muster_call_async(&node[0], T_COUNT, args, sizeof args);
            rig_step(1);
            if (!node[2].joined) live = 0;
        }
        if (h2 >= 0) muster_cancel(&node[0], (int8_t)h2);
        CHECK(live && accepted[2] == before,
              "lead keeps broadcasting heartbeats while busy with unicast traffic");
    }

    lost_events = 0; joined_events = 0;
    bus.ports[1].muted = 1;                           /* W1 hears, can't speak */
    rig_step(1500);
    CHECK(lost_events == 1 && muster_alive_count(&node[0]) == 2, "silent worker declared lost");
    CHECK(node[1].joined, "...while it still believes it is joined");
    bus.ports[1].muted = 0;
    rig_step(800);
    CHECK(muster_alive_count(&node[0]) == 3 && joined_events == 1,
          "lead tells the returning worker to rejoin and re-admits it");

    start_lead();                                     /* lead reboots */
    rig_step(1000);
    CHECK(muster_alive_count(&node[0]) == 3 && node[1].joined && node[2].joined,
          "after a lead restart every worker rejoins");
    h = muster_call_async(&node[2], T_COUNT, args, sizeof args);
    st = rig_wait(&node[2], h, out, sizeof out, &n, 2000);
    CHECK(st == MUSTER_OK && sum_of(out, n) == 10, "calls work again after the lead restart");

    printf("5. hostile or broken input\n");
    m_send_call(&node[2], 0, 2, 99, 200, 0, 0);      /* task ID 200 */
    rig_step(5);
    CHECK(g_seen_call == 99 && g_seen_status == (uint8_t)MUSTER_ENOHOST,
          "CALL with an out-of-range task ID gets ENOHOST (no out-of-bounds read)");
    node[2].resident |= 0x80000000ul;
    m_send_join(&node[2]);
    rig_step(5);
    node[2].resident &= ~0x80000000ul;
    {
        const muster_node *nano = muster_node_by_id(&node[0], 2);
        CHECK(nano && !(nano->resident & 0x80000000ul) && (nano->resident & (1ul << T_SENSOR)),
              "JOIN advertising task IDs beyond MUSTER_MAX_TASKS is masked");
    }

    printf("6. API edges\n");
    h = muster_call_async(&node[0], T_COUNT, args, sizeof args);
    {
        const muster_node *w1 = muster_node_by_id(&node[0], 1);
        int queued_before = w1 && w1->queued > 0;
        CHECK(last_placed == 1 && queued_before && w1->queued == 50,
              "a task the lead does not hold still adds its cost to the worker's queue");
        muster_cancel(&node[0], h);
        CHECK(w1->queued == 0 && w1->inflight == 0 && !node[0].pend[h].used,
              "cancel frees the handle and gives back the node's load");
    }
    rig_step(20);

    h = muster_call_async(&node[0], T_NEST, 0, 0);
    st = rig_wait(&node[0], h, out, sizeof out, &n, 100);
    CHECK(st == MUSTER_OK && g_nest_rc == MUSTER_ERR,
          "muster_call from inside a task is refused instead of re-entering the core");

    h = muster_call_async(&node[0], T_BIG, 0, 0);
    n = 0;
    st = rig_wait(&node[0], h, out, 8, &n, 100);
    CHECK(st == MUSTER_ERR && n == 40, "result larger than the caller's buffer: ERR and the size needed");

    CHECK(muster_call_async(&node[0], T_COUNT, out, MUSTER_MAX_ARGS + 1) == MUSTER_ERR,
          "arguments longer than MUSTER_MAX_ARGS are refused");

    printf("\n%u frames on the bus.", (unsigned)bus.frames_sent);
    return rig_done();
}

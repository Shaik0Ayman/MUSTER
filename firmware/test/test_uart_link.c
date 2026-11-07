/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_uart_link.c - the byte-stream link: framing, resync after corruption,
 * and a real lead + two workers star over simulated cables (one link each). */
#define MUSTER_MAX_LINKS 2
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_uart.h"
#include "rig.h"

/* a cable: one byte queue per direction; `dead` loses everything on it */
typedef struct { uint8_t q[2][2048]; uint16_t head[2], tail[2]; int corrupt_next, dead; } cable;
typedef struct { cable *c; int side; } end;       /* side 0 writes q[0], reads q[1] */

static void w(void *io, const uint8_t *p, uint16_t n)
{
    end *e = (end *)io; uint16_t i;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (e->c->dead) continue;
        if (e->c->corrupt_next && i == 3) { b ^= 0x40; e->c->corrupt_next = 0; }
        e->c->q[e->side][e->c->tail[e->side]++ % 2048] = b;
    }
}
static int r(void *io)
{
    end *e = (end *)io; int o = 1 - e->side;
    if (e->c->head[o] == e->c->tail[o]) return -1;
    return e->c->q[o][e->c->head[o]++ % 2048];
}

enum { T_ADD = 1 };
static int runs[RIG_MAX];
static int16_t add_tr(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_rbuf rb; muster_wbuf wb; int16_t x, y;
    if (rig_cur >= 0) runs[rig_cur]++;
    muster_r_init(&rb, a, len); x = muster_get_i16(&rb); y = muster_get_i16(&rb);
    if (rb.err) return -1;
    muster_w_init(&wb, ret, cap); muster_put_i16(&wb, (int16_t)(x + y));
    return wb.err ? -1 : (int16_t)wb.n;
}

static cable cab[2];
static end lead_end[2], wrk_end[2];
static muster_uart_ctx lctx[2], wctx[2];
static muster_link llink[2], wlink[2];
static muster_t lead, wk[2];

static int call_add(int16_t x, int16_t y, int16_t *out)
{
    uint8_t a[4], o[4]; uint16_t n = 0; muster_wbuf wb; int8_t st;
    muster_w_init(&wb, a, sizeof a); muster_put_i16(&wb, x); muster_put_i16(&wb, y);
    st = rig_wait(&lead, muster_call_async(&lead, T_ADD, a, wb.n), o, sizeof o, &n, 800);
    if (st == MUSTER_OK) { muster_rbuf rb; muster_r_init(&rb, o, n); *out = muster_get_i16(&rb); }
    return st;
}

int main(void)
{
    uint8_t f[40], got[40], src = 99, noise[5] = {0x00, 0x7E, 0x00, 0x7E, 0xFF};
    uint16_t n; int i, done; int16_t sum;
    muster_link la, lb; muster_uart_ctx ca, cb_; end ea, eb; static cable one;

    printf("\nUART link tests\n\n");
    printf("1. framing\n");
    ea.c = eb.c = &one; ea.side = 0; eb.side = 1;
    muster_uart_attach(&la, &ca, &ea, w, r, 5, 115);      /* la sends; its far end is node 5   */
    muster_uart_attach(&lb, &cb_, &eb, w, r, 7, 115);     /* lb receives; its far end is node 7 */
    for (i = 0; i < 20; i++) f[i] = (uint8_t)(i + 1);
    CHECK(muster_uart_send(&la, 0, f, 20) == 0, "a frame is sent");
    n = muster_uart_recv(&lb, &src, got, sizeof got);
    CHECK(n == 20 && memcmp(f, got, 20) == 0 && src == 7, "and arrives intact, tagged with the peer ID");
    CHECK(muster_uart_recv(&lb, &src, got, sizeof got) == 0, "recv does not block when empty");
    CHECK(muster_uart_send(&la, 0, f, MUSTER_UART_MTU + 1) == -1 && muster_uart_send(&la, 0, f, 0) == -1,
          "oversized and empty frames are refused");
    one.corrupt_next = 1;
    muster_uart_send(&la, 0, f, 20);                       /* one byte flipped in transit */
    muster_uart_send(&la, 0, f, 10);
    n = muster_uart_recv(&lb, &src, got, sizeof got);
    CHECK(n == 10 && memcmp(f, got, 10) == 0, "a corrupted frame is dropped; the next one still arrives");
    w(&ea, noise, sizeof noise);
    muster_uart_send(&la, 0, f, 6);
    n = muster_uart_recv(&lb, &src, got, sizeof got);
    CHECK(n == 6 && memcmp(f, got, 6) == 0, "line noise before a frame is skipped");

    printf("2. a star: one lead, two workers, one cable each\n");
    for (i = 0; i < 2; i++) {
        lead_end[i].c = wrk_end[i].c = &cab[i]; lead_end[i].side = 0; wrk_end[i].side = 1;
        muster_uart_attach(&llink[i], &lctx[i], &lead_end[i], w, r, (uint8_t)(i + 1), 115);
        muster_uart_attach(&wlink[i], &wctx[i], &wrk_end[i], w, r, 0, 115);
    }
    muster_begin(&lead, 0, 1, 2, 512, 240, MUSTER_CAP_FPU, &llink[0], now_ms);
    CHECK(muster_add_link(&lead, &llink[1]) == 1, "the lead takes a second link");
    muster_set_cost(&lead, T_ADD, 10);
    for (i = 0; i < 2; i++) {
        muster_begin(&wk[i], (uint8_t)(i + 1), 0, 2, 512, 240, 0, &wlink[i], now_ms);
        muster_register_task(&wk[i], T_ADD, add_tr, 10);
        rig_add(&wk[i]);
    }
    rig_add(&lead);                                        /* rig_cur: 0, 1 = workers, 2 = lead */
    rig_step(800);
    CHECK(muster_alive_count(&lead) == 3, "both workers join over their own cable");
    CHECK(call_add(40, 2, &sum) == MUSTER_OK && sum == 42, "a call is placed on a worker and returns");

    cab[0].dead = 1;                                       /* pull worker 1's cable */
    rig_step(MUSTER_NODE_TIMEOUT_MS + 600);
    CHECK(muster_alive_count(&lead) == 2, "a pulled cable: the lead loses that worker");
    memset(runs, 0, sizeof runs);
    for (done = 0, i = 0; i < 3; i++) done += (call_add((int16_t)i, 1, &sum) == MUSTER_OK && sum == i + 1);
    CHECK(done == 3 && runs[0] == 0 && runs[1] == 3, "calls keep completing, all on the other worker");

    cab[0].dead = 0;                                       /* plug it back */
    rig_step(1500);
    CHECK(muster_alive_count(&lead) == 3, "plugged back: the worker rejoins");
    return rig_done();
}

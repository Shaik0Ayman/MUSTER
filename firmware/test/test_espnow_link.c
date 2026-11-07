/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_espnow_link.c - the ESP-NOW link's ring and packet logic, then a real
 * lead + two workers on a simulated lossy broadcast medium (every board hears
 * every frame, like the radio). The ESP32 radio glue itself is not tested here. */
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_espnow.h"
#include "rig.h"

/* ---- the "air": a send hook that hands the packet to every other board ---- */
#define NB 3
static mespnow_ctx ctx[NB];
static muster_link lnk[NB];
static muster_t    nd[NB];
static int  loss_pct;           /* each delivery is dropped with this probability */
static uint32_t rng = 12345;
static uint32_t sent_pkts, lost_pkts;
static uint8_t  last_src_byte;  /* first byte of the most recent packet on the air */

static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 16; }

static int air_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t n)
{
    mespnow_ctx *me = (mespnow_ctx *)l->ctx;
    uint8_t pkt[MUSTER_ESPNOW_MTU + 1]; uint16_t len; int i;
    (void)dst;
    len = mespnow_pack(me, f, n, pkt);
    if (!len) return -1;
    last_src_byte = pkt[0];
    sent_pkts++;
    for (i = 0; i < NB; i++) {
        if (&ctx[i] == me) continue;                  /* a radio does not hear itself */
        if ((int)(rnd() % 100) < loss_pct) { lost_pkts++; continue; }
        mespnow_push(&ctx[i], pkt, len);
    }
    return 0;
}

enum { T_MUL = 1 };
static int runs[RIG_MAX];
static int16_t mul_tr(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_rbuf r; muster_wbuf w; int16_t x, y;
    if (rig_cur >= 0) runs[rig_cur]++;
    muster_r_init(&r, a, len); x = muster_get_i16(&r); y = muster_get_i16(&r);
    if (r.err) return -1;
    muster_w_init(&w, ret, cap); muster_put_i32(&w, (int32_t)x * y);
    return w.err ? -1 : (int16_t)w.n;
}

static int call_mul(int16_t x, int16_t y, int32_t *out)
{
    uint8_t a[4], o[8]; uint16_t n = 0; muster_wbuf w; int8_t st;
    muster_w_init(&w, a, sizeof a); muster_put_i16(&w, x); muster_put_i16(&w, y);
    st = rig_wait(&nd[0], muster_call_async(&nd[0], T_MUL, a, w.n), o, sizeof o, &n, 3000);
    if (st == MUSTER_OK) { muster_rbuf r; muster_r_init(&r, o, n); *out = muster_get_i32(&r); }
    return st;
}

int main(void)
{
    mespnow_ctx c, d; uint8_t pkt[MUSTER_ESPNOW_MTU + 1], f[MUSTER_ESPNOW_MTU], src = 0, in[10];
    uint16_t n; int i, ok; int32_t v;

    printf("\nESP-NOW link tests\n\n");
    printf("1. packet and ring\n");
    memset(&c, 0, sizeof c); memset(&d, 0, sizeof d);
    c.self = 7; d.self = 9;
    for (i = 0; i < 10; i++) in[i] = (uint8_t)(0xA0 + i);
    n = mespnow_pack(&c, in, 10, pkt);
    CHECK(n == 11 && pkt[0] == 7 && memcmp(pkt + 1, in, 10) == 0, "a packet is the sender's ID then the frame");
    CHECK(mespnow_pack(&c, in, 0, pkt) == 0 && mespnow_pack(&c, in, MUSTER_ESPNOW_MTU + 1, pkt) == 0,
          "empty and oversized frames are refused");
    mespnow_push(&d, pkt, 11);
    n = mespnow_pop(&d, &src, f, sizeof f);
    CHECK(n == 10 && src == 7 && memcmp(f, in, 10) == 0, "it comes out of the receiver's ring tagged with the source");
    CHECK(mespnow_pop(&d, &src, f, sizeof f) == 0, "pop on an empty ring returns 0 at once");
    mespnow_push(&c, pkt, 11);                           /* pkt[0] == c.self: our own echo */
    CHECK(mespnow_pop(&c, &src, f, sizeof f) == 0, "a board ignores packets carrying its own ID");
    mespnow_push(&d, pkt, 1);
    mespnow_push(&d, pkt, 0);
    mespnow_push(&d, pkt, MUSTER_ESPNOW_MTU + 2);
    CHECK(mespnow_pop(&d, &src, f, sizeof f) == 0, "runt and oversized packets are dropped");
    for (i = 0; i < MUSTER_ESPNOW_RING + 5; i++) { pkt[1] = (uint8_t)i; mespnow_push(&d, pkt, 11); }
    CHECK(d.dropped == 6, "a full ring drops the overflow and counts it (one slot stays empty)");
    ok = 1;
    for (i = 0; i < MUSTER_ESPNOW_RING - 1; i++)
        if (mespnow_pop(&d, &src, f, sizeof f) != 10 || f[0] != (uint8_t)i) ok = 0;
    CHECK(ok && mespnow_pop(&d, &src, f, sizeof f) == 0, "what was kept comes out in order");
    pkt[1] = 1; mespnow_push(&d, pkt, 11);
    CHECK(mespnow_pop(&d, &src, f, 4) == 0 && mespnow_pop(&d, &src, f, sizeof f) == 0,
          "a frame bigger than the caller's buffer is discarded, not truncated");

    printf("2. a lead and two workers on the air\n");
    for (i = 0; i < NB; i++) mespnow_attach_raw(&lnk[i], &ctx[i], (uint8_t)i, air_send);
    muster_begin(&nd[0], 0, 1, 2, 512, 160, MUSTER_CAP_FPU, &lnk[0], now_ms);
    muster_set_cost(&nd[0], T_MUL, 50);
    for (i = 1; i < NB; i++) {
        muster_begin(&nd[i], (uint8_t)i, 0, 2, 512, 240, MUSTER_CAP_FPU, &lnk[i], now_ms);
        muster_set_uid(&nd[i], 0xC0DE0000ul + 0x1111ul * (uint32_t)i);   /* shared-image style ID */
        muster_register_task(&nd[i], T_MUL, mul_tr, 50);
        rig_add(&nd[i]);
    }
    rig_add(&nd[0]);                                    /* rig_cur: 0, 1 = workers, 2 = lead */
    rig_step(1000);
    CHECK(muster_alive_count(&nd[0]) == 3, "both workers join over the shared medium");
    CHECK(ctx[1].self == nd[1].id && ctx[2].self == nd[2].id && nd[1].id != 1,
          "after muster_set_uid the link puts the new node ID on the air");
    CHECK(call_mul(123, -45, &v) == MUSTER_OK && v == -5535, "a call is placed on a worker and returns");

    printf("3. 10%% of deliveries lost\n");
    loss_pct = 10; memset(runs, 0, sizeof runs); lost_pkts = 0;
    for (ok = 0, i = 0; i < 100; i++) ok += (call_mul((int16_t)i, 3, &v) == MUSTER_OK && v == i * 3);
    CHECK(ok >= 98, "at least 98 of 100 calls still return the right answer (retries recover the rest)");
    CHECK(lost_pkts > 20, "and packets really were being lost");
    CHECK(runs[0] + runs[1] >= 100, "the workers ran every call");

    printf("4. a worker leaves\n");
    loss_pct = 0;                                       /* worker 1 goes quiet: stop polling it */
    rig_count = 0; rig_add(&nd[2]); rig_add(&nd[0]);
    rig_step(MUSTER_NODE_TIMEOUT_MS + 500);
    CHECK(muster_alive_count(&nd[0]) == 2, "the lead notices it is silent");
    memset(runs, 0, sizeof runs);
    for (ok = 0, i = 0; i < 5; i++) ok += (call_mul(i, 7, &v) == MUSTER_OK && v == i * 7);
    CHECK(ok == 5 && runs[0] == 5, "calls keep completing on the remaining worker");
    return rig_done();
}

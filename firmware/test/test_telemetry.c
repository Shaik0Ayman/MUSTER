/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_telemetry.c - the lead's optional telemetry (MUSTER_TELEMETRY) and its
 * snapshot frame. A lead and two workers on an 8-byte bus:
 *   node 1  worker  holds T_WORK, takes 3 ms per call
 *   node 2  worker  holds T_WORK, takes 1 ms per call
 * `./test_telemetry out.bin` also writes one frame for the Python decoder test.
 */
#define MUSTER_TELEMETRY 1
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_loopback.h"
#include "rig.h"

enum { T_WORK = 1 };

static lb_bus      bus;
static lb_ctx      ctx[3];
static muster_link link_[3];
static muster_t    node[3];

static uint32_t now_us(void) { return g_now * 1000u + 7u; }

static int16_t tr_work(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_wbuf w;
    (void)a; (void)len;
    rig_step(rig_cur == 1 ? 3 : 1);      /* "compute" while the others keep polling */
    muster_w_init(&w, ret, cap); muster_put_u8(&w, 1);
    return w.err ? -1 : (int16_t)w.n;
}

/* ------------------------------------------------ a minimal decoder */
typedef struct { uint8_t id, alive; uint32_t uid, ok, failed, retries, busy, last, avg, mn, mx; } dnode;
typedef struct {
    uint16_t seq; uint32_t t_ms; uint8_t lead, nlinks, nnodes, has_us;
    uint32_t ok, failed, retries, d_last, d_avg, d_max; uint16_t hist[8];
    uint32_t tx_frames[8], rx_frames[8], tx_bytes[8];
    dnode n[8]; uint8_t nev, ev[32];
} snap;

static int decode(const uint8_t *b, uint16_t len, snap *s)
{
    muster_rbuf r; uint16_t plen, crc; uint8_t i, k;
    if (len < 7 || b[0] != 'M' || b[1] != 'T' || b[2] != MUSTER_TEL_VERSION) return -1;
    plen = (uint16_t)(b[3] | (b[4] << 8));
    if ((uint16_t)(plen + 7u) != len) return -2;
    crc = (uint16_t)(b[len - 2] | (b[len - 1] << 8));
    if (crc != muster_crc16(b + 2, (uint16_t)(len - 4u))) return -3;
    muster_r_init(&r, b + 5, plen);
    s->seq = muster_get_u16(&r); s->t_ms = muster_get_u32(&r);
    s->lead = muster_get_u8(&r); s->nlinks = muster_get_u8(&r);
    s->nnodes = muster_get_u8(&r); s->has_us = muster_get_u8(&r);
    s->ok = muster_get_u32(&r); s->failed = muster_get_u32(&r); s->retries = muster_get_u32(&r);
    s->d_last = muster_get_u32(&r); s->d_avg = muster_get_u32(&r); s->d_max = muster_get_u32(&r);
    for (k = 0; k < 8; k++) s->hist[k] = muster_get_u16(&r);
    for (i = 0; i < s->nlinks && i < 8; i++) {
        muster_get_u16(&r); muster_get_u16(&r);
        s->tx_frames[i] = muster_get_u32(&r); s->tx_bytes[i] = muster_get_u32(&r);
        s->rx_frames[i] = muster_get_u32(&r); muster_get_u32(&r); muster_get_u32(&r);
    }
    for (i = 0; i < s->nnodes && i < 8; i++) {
        dnode *d = &s->n[i];
        d->id = muster_get_u8(&r); d->alive = muster_get_u8(&r);
        muster_get_u8(&r); muster_get_u8(&r); muster_get_u8(&r);
        muster_get_u16(&r); muster_get_u16(&r);
        d->uid = muster_get_u32(&r); muster_get_u32(&r);
        d->ok = muster_get_u32(&r); d->failed = muster_get_u32(&r); d->retries = muster_get_u32(&r);
        d->busy = muster_get_u32(&r); d->last = muster_get_u32(&r); d->avg = muster_get_u32(&r);
        d->mn = muster_get_u32(&r); d->mx = muster_get_u32(&r);
    }
    s->nev = muster_get_u8(&r);
    for (i = 0; i < s->nev && i < 32; i++) {
        muster_get_u16(&r); muster_get_u32(&r);
        s->ev[i] = muster_get_u8(&r); muster_get_u8(&r); muster_get_u8(&r);
    }
    return (r.err || r.n != plen) ? -4 : 0;
}

static const dnode *by_id(const snap *s, uint8_t id)
{
    uint8_t i;
    for (i = 0; i < s->nnodes; i++) if (s->n[i].id == id) return &s->n[i];
    return 0;
}

static int run_calls(int count)
{
    int i, ok = 0; uint8_t out[8]; uint16_t n;
    for (i = 0; i < count; i++) {
        int8_t h = muster_call_async(&node[0], T_WORK, 0, 0);
        if (rig_wait(&node[0], h, out, sizeof out, &n, 3000) == MUSTER_OK) ok++;
    }
    return ok;
}

/* Two calls at once, so placement spreads them over both workers. */
static int run_pairs(int pairs)
{
    int i, ok = 0; uint8_t out[8]; uint16_t n;
    for (i = 0; i < pairs; i++) {
        int8_t h1 = muster_call_async(&node[0], T_WORK, 0, 0);
        int8_t h2 = muster_call_async(&node[0], T_WORK, 0, 0);
        if (rig_wait(&node[0], h1, out, sizeof out, &n, 3000) == MUSTER_OK) ok++;
        if (rig_wait(&node[0], h2, out, sizeof out, &n, 3000) == MUSTER_OK) ok++;
    }
    return ok;
}

int main(int argc, char **argv)
{
    static uint8_t buf[MUSTER_TEL_FRAME_MAX];
    snap s, s2;
    uint16_t len;
    int i, rc, has_lost = 0;
    uint32_t sum_ok = 0, hist = 0;

    memset(&bus, 0, sizeof bus);
    for (i = 0; i < 3; i++) lb_attach(&bus, &ctx[i], &link_[i], (uint8_t)i, 8, 500);
    muster_begin(&node[0], 0, 1, 2, 512, 240, MUSTER_CAP_FPU, &link_[0], now_ms);
    muster_begin(&node[1], 1, 0, 2, 512, 240, MUSTER_CAP_FPU, &link_[1], now_ms);
    muster_begin(&node[2], 2, 0, 2, 512, 240, MUSTER_CAP_FPU, &link_[2], now_ms);
    muster_set_micros(&node[0], now_us);
    muster_set_cost(&node[0], T_WORK, 10);
    for (i = 1; i < 3; i++) muster_register_task(&node[i], T_WORK, tr_work, 10);
    for (i = 0; i < 3; i++) rig_add(&node[i]);

    printf("1. snapshot frame\n");
    CHECK(muster_telemetry_frame(&node[0], buf, (uint16_t)(MUSTER_TEL_FRAME_MAX - 1u)) == 0,
          "a buffer smaller than MUSTER_TEL_FRAME_MAX is refused");
    CHECK(muster_telemetry_frame(&node[1], buf, sizeof buf) == 0, "a worker emits nothing");
    rig_step(400);
    len = muster_telemetry_frame(&node[0], buf, sizeof buf);
    rc = decode(buf, len, &s);
    CHECK(len > 0 && len <= MUSTER_TEL_FRAME_MAX && rc == 0, "the lead's frame decodes (magic, length, CRC)");
    CHECK(s.lead == 0 && s.nlinks == 1 && s.nnodes == 3 && s.has_us == 1,
          "it names the lead, its link, all three boards and the us clock");
    CHECK(by_id(&s, 1) && by_id(&s, 1)->alive && by_id(&s, 2) && by_id(&s, 2)->alive, "both workers alive");
    CHECK(s.tx_frames[0] > 0 && s.rx_frames[0] > 0 && s.tx_bytes[0] >= s.tx_frames[0],
          "link traffic is counted (joins and heartbeats)");
    buf[10] ^= 0x01;
    CHECK(decode(buf, len, &s2) == -3, "a flipped bit fails the CRC");

    printf("2. calls, latency, load\n");
    CHECK(run_pairs(10) == 20, "20 calls complete (two in flight at a time)");
    len = muster_telemetry_frame(&node[0], buf, sizeof buf);
    CHECK(decode(buf, len, &s2) == 0 && s2.seq == (uint16_t)(s.seq + 1u), "sequence number advances");
    for (i = 0; i < s2.nnodes; i++) sum_ok += s2.n[i].ok;
    for (i = 0; i < 8; i++) hist += s2.hist[i];
    CHECK(s2.ok == 20 && s2.failed == 0 && sum_ok == 20 && hist == 20,
          "20 calls end to end = 20 attempts across boards = 20 in the histogram");
    {
        const dnode *a = by_id(&s2, 1), *b = by_id(&s2, 2);
        CHECK(a && b && a->ok + b->ok == 20 && a->ok > 0 && b->ok > 0,
              "every attempt is credited to the board that ran it (both did work)");
        if (b) printf("        node 2: ok=%lu min=%lu avg=%lu max=%lu last=%lu\n", (unsigned long)b->ok,
                      (unsigned long)b->mn, (unsigned long)b->avg, (unsigned long)b->mx, (unsigned long)b->last);
        if (a) printf("        node 1: ok=%lu min=%lu avg=%lu max=%lu\n", (unsigned long)a->ok,
                      (unsigned long)a->mn, (unsigned long)a->avg, (unsigned long)a->mx);
        CHECK(b && b->ok > 0 && b->mn >= 1000 && b->mx >= b->mn && b->avg >= b->mn && b->avg <= b->mx,
              "per-board latency min <= avg <= max, in us");
        CHECK(b && b->busy >= b->ok * b->mn, "busy time adds up the attempts");
        CHECK(s2.d_avg >= 1000 && s2.d_max >= s2.d_last && s2.d_max >= s2.d_avg,
              "end-to-end delay is recorded");
        CHECK(a && b && a->mn > b->mx, "the slower board (3 ms per call) shows the higher latency");
    }

    printf("3. a board goes silent\n");
    bus.ports[2].unplugged = 1;
    bus.ports[1].unplugged = 1;
    rc = run_calls(1);
    rig_step(1500);
    len = muster_telemetry_frame(&node[0], buf, sizeof buf);
    CHECK(decode(buf, len, &s2) == 0, "frame decodes after the loss");
    for (i = 0; i < s2.nev; i++) if (s2.ev[i] == MUSTER_EV_LOST) has_lost = 1;
    CHECK(has_lost, "the LOST event is in the snapshot's recent events");
    bus.ports[1].unplugged = 0;
    rig_step(500);
    CHECK(run_calls(3) == 3, "calls work again once a board is back");
    len = muster_telemetry_frame(&node[0], buf, sizeof buf);
    CHECK(decode(buf, len, &s2) == 0, "frame still decodes");
    CHECK(s2.retries >= 1 && s2.failed == (uint32_t)(1 - rc), "retries and the failed call are counted");
    CHECK(s2.nev == MUSTER_TEL_EVENTS, "the event list is capped at MUSTER_TEL_EVENTS");
    CHECK(!by_id(&s2, 2)->alive, "the unplugged board shows as not alive");

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (f) { fwrite(buf, 1, len, f); fclose(f); }
        printf("  wrote %u-byte frame to %s\n", (unsigned)len, argv[1]);
    }
    return rig_done();
}

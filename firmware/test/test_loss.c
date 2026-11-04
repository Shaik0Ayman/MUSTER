/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_loss.c - ROADMAP M1 loss test: 5% of frame deliveries are dropped at
 * random (a receiver's buffer overflowing) on an 8-byte-MTU bus, and every
 * call must still complete with the right answer.
 *
 * A 24-byte call crosses about 8 frames each way, so one attempt fails about
 * a third of the time at this loss rate. MUSTER_MAX_RETRIES is raised to 6
 * here so the retry budget, not luck, carries every call; with the default 2
 * an application would see occasional ETIMEOUTs at 5% loss.               */
#define MUSTER_MAX_RETRIES 6u
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_loopback.h"
#include "rig.h"
#include <math.h>

enum { T_IMU = 1, T_SENSOR = 2 };

static lb_bus      bus;
static lb_ctx      ctx[3];
static muster_link link_[3];
static muster_t    node[3];
static int         runs, lost;

static int16_t tr_imu(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_rbuf r; muster_wbuf w; float v[6]; int i;
    runs++;
    muster_r_init(&r, a, len);
    for (i = 0; i < 6; i++) v[i] = muster_get_f32(&r);
    if (r.err) return -1;
    muster_w_init(&w, ret, cap);
    for (i = 0; i < 3; i++) muster_put_f32(&w, 0.98f * v[3 + i] + 0.02f * v[i]);
    return w.err ? -1 : (int16_t)w.n;
}

static void on_event(uint8_t ev, uint8_t a, uint8_t b, void *u)
{ (void)a; (void)b; (void)u; if (ev == MUSTER_EV_LOST) lost++; }

static int run_calls(muster_t *from, int count, int *ok_out)
{
    uint8_t args[24], out[16]; uint16_t n = 0; int i, k, ok = 0;
    for (k = 0; k < count; k++) {
        muster_wbuf w; muster_rbuf r; float x;
        muster_w_init(&w, args, sizeof args);
        for (i = 0; i < 6; i++) muster_put_f32(&w, (float)(k + i));
        n = 0;
        if (rig_wait(from, muster_call_async(from, T_IMU, args, w.n), out, sizeof out, &n, 10000)
            != MUSTER_OK) continue;
        muster_r_init(&r, out, n);
        x = muster_get_f32(&r);
        if (!r.err && fabsf(x - (0.98f * (float)(k + 3) + 0.02f * (float)k)) < 1e-3f) ok++;
        rig_step(3);
    }
    *ok_out = ok;
    return ok == count;
}

int main(void)
{
    int i, ok;
    printf("\nloss test - 5%% random frame loss, 8-byte frames\n\n");
    for (i = 0; i < 3; i++) lb_attach(&bus, &ctx[i], &link_[i], (uint8_t)i, 8, 500);
    muster_begin(&node[0], 0, 1, 2, 512, 240, MUSTER_CAP_FPU, &link_[0], now_ms);
    muster_begin(&node[1], 1, 0, 2, 512, 240, MUSTER_CAP_FPU, &link_[1], now_ms);
    muster_begin(&node[2], 2, 0, 1, 2, 16, 0, &link_[2], now_ms);
    muster_register_task(&node[0], T_IMU, tr_imu, 300);
    muster_register_task(&node[1], T_IMU, tr_imu, 300);
    muster_register_task(&node[2], T_SENSOR, tr_imu, 20);
    muster_on_event(&node[0], on_event, 0);
    for (i = 0; i < 3; i++) rig_add(&node[i]);

    bus.drop_permille = 50;
    bus.rng = 12345;
    rig_step(1500);
    CHECK(muster_alive_count(&node[0]) == 3, "all boards joined despite loss");

    runs = 0;
    CHECK(run_calls(&node[0], 200, &ok), "200 calls from the lead all completed correctly");
    printf("        %d/200 ok, %d executions\n", ok, runs);
    runs = 0;
    CHECK(run_calls(&node[2], 100, &ok), "100 calls from the Nano (via the lead) all completed correctly");
    printf("        %d/100 ok, %d executions\n", ok, runs);
    CHECK(lost == 0, "no board was wrongly declared lost");
    printf("        %lu of %lu frame deliveries dropped\n",
           (unsigned long)bus.frames_dropped, (unsigned long)bus.frames_sent * 2ul);
    return rig_done();
}

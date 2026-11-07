/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_trio_task.c - the three-board demo's task (examples/trio): the series
 * converges to the closed form, the result survives the wire encoding, and the
 * task drives the PWM hook with the duty it reports. */
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "../examples/trio/muster_trio/trio_task.h"
#include <stdio.h>

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("  PASS  %s\n", msg); \
                              else { printf("  FAIL  %s\n", msg); fails++; } } while (0)

static uint32_t fake_us;
static uint8_t  pwm_seen = 0xEE;
static uint32_t us(void) { return fake_us += 700; }
static void fill(trio_result *r)
{
    r->node = 42; r->chip = TRIO_CHIP_ESP32; r->cpu_mhz = 240;
    r->free_heap = 200000; r->uptime_ms = 123456; r->uid = 0xDEADBEEFul;
}
static void pwm(uint8_t d) { pwm_seen = d; }

int main(void)
{
    uint16_t s; uint8_t d, a[4], out[MUSTER_MAX_RET]; int ok = 1, bad = 0; float worst = 0, e;
    muster_wbuf w; trio_result r; int16_t n; const float pi = 3.14159265f;

    printf("\ntrio demo task tests\n\n");
    printf("1. the math\n");
    for (s = 1; s < TRIO_PERIOD; s++) {                    /* step 0 is the jump itself */
        float x = 2.0f * pi * (float)s / (float)TRIO_PERIOD;
        if (x < 0.4f || x > 2.0f * pi - 0.4f) continue;    /* the series rings near the jump (Gibbs) */
        e = fabsf(trio_wave(s, TRIO_TERMS, &d) - (pi - x) / 2.0f);
        if (e > worst) worst = e;
        if (e > 0.05f) bad++;
    }
    printf("        worst error vs (pi - x)/2 away from the jump: %.4f\n", (double)worst);
    CHECK(bad == 0, "the series matches the closed form (pi - x)/2 within 0.05");
    trio_wave(TRIO_PERIOD / 2, TRIO_TERMS, &d);
    CHECK(d >= 126 && d <= 129, "half way round the cycle the duty is about 50%");
    trio_wave(TRIO_PERIOD / 16, TRIO_TERMS, &d);
    CHECK(d > 200, "early in the cycle it is bright...");
    trio_wave(TRIO_PERIOD - TRIO_PERIOD / 16, TRIO_TERMS, &d);
    CHECK(d < 55, "...and late in the cycle it is dim");
    for (s = 0; s < 3 * TRIO_PERIOD; s++) {
        float v = trio_wave(s, TRIO_TERMS, &d);
        if (!(v == v) || fabsf(v) > 2.0f) ok = 0;   /* Gibbs overshoot peaks near 1.85 */
    }
    CHECK(ok, "no NaN and the sum stays bounded (|v| < 2) over three cycles");

    printf("2. as a Muster task\n");
    trio_hooks.micros = us; trio_hooks.fill = fill; trio_hooks.pwm = pwm;
    muster_w_init(&w, a, sizeof a); trio_encode_args(&w, 32, TRIO_TERMS);
    n = trio_wave_tr(a, w.n, out, sizeof out);
    CHECK(n > 0 && n < 40, "the result is small enough for one radio frame");
    CHECK(n == (int16_t)TRIO_RESULT_BYTES, "the result is exactly TRIO_RESULT_BYTES long");
    CHECK(trio_get_result(out, (uint16_t)n, &r) == 0, "it decodes");
    CHECK(r.node == 42 && r.chip == TRIO_CHIP_ESP32 && r.cpu_mhz == 240 && r.uid == 0xDEADBEEFul &&
          r.free_heap == 200000 && r.uptime_ms == 123456, "the board's vital signs arrive intact");
    CHECK(r.busy_us == 700 && r.tasks == 1, "execution time and task count are filled in by the task");
    CHECK(pwm_seen == r.duty && r.duty > 0, "the PWM hook got exactly the duty that was reported");
    CHECK(fabsf(r.value - trio_wave(32, TRIO_TERMS, &d)) < 1e-6f && r.duty == d, "same step, same answer on every board");
    trio_wave_tr(a, w.n, out, sizeof out);
    trio_get_result(out, (uint16_t)trio_wave_tr(a, w.n, out, sizeof out), &r);
    CHECK(r.tasks == 3, "the count keeps going");
    CHECK(trio_wave_tr(a, 2, out, sizeof out) == -1, "short arguments are refused");
    muster_w_init(&w, a, sizeof a); trio_encode_args(&w, 1, 0);
    CHECK(trio_wave_tr(a, w.n, out, sizeof out) == -1, "zero terms is refused");
    muster_w_init(&w, a, sizeof a); trio_encode_args(&w, 1, 8);
    CHECK(trio_wave_tr(a, w.n, out, 4) == -1, "a result buffer that is too small is refused, not overrun");
    printf("\n%s\n\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fails ? 1 : 0;
}

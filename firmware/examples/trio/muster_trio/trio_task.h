/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* trio_task.h - the one task of the three-board demo, and what travels with it.
 *
 * wave(step, terms): a Fourier series, summed in floating point on whichever
 * board the lead picks:
 *
 *     value = sum_{k=1..terms} sin(k*x) / k      x = 2*pi*(step mod PERIOD)/PERIOD
 *
 * which converges to (pi - x)/2 on (0, 2*pi): a sawtooth. The board maps it to a
 * PWM duty (0..255) and drives its pin, so every board's LED ramps as the steps
 * advance, and the result can be checked against the closed form.
 *
 * Alongside the answer the board returns its own vital signs, so the lead can
 * build a per-board dashboard purely from results - no extra protocol.
 *
 * No Arduino dependency here: the sketches fill in `trio_hooks`, and
 * test_trio_task.c runs the same code on the host.
 */
#ifndef TRIO_TASK_H
#define TRIO_TASK_H
#include <math.h>
#include "muster.h"

#define TRIO_T_WAVE 1
#define TRIO_PERIOD 128u /* steps per sawtooth cycle */
#define TRIO_TERMS 256u  /* series terms: ~1 ms on an ESP32 FPU, much slower with no FPU (C6) */
#define TRIO_COST 50     /* placement cost units */

enum { TRIO_CHIP_UNKNOWN = 0, TRIO_CHIP_ESP32 = 1, TRIO_CHIP_C6 = 2, TRIO_CHIP_S3 = 3 };

/* Everything a board reports about itself with each result. Wire layout is
 * fixed-width little-endian (muster_put_*), 31 bytes after the value. */
typedef struct
{
    uint8_t node;       /* muster node ID of the board that ran it             */
    uint8_t chip;       /* TRIO_CHIP_*                                         */
    uint8_t duty;       /* PWM duty it applied, 0..255                         */
    uint16_t cpu_mhz;
    float value;        /* the series sum                                      */
    uint32_t busy_us;   /* how long this call took                             */
    uint32_t tasks;     /* calls this board has run since boot                 */
    uint32_t free_heap; /* bytes                                               */
    uint32_t uptime_ms;
    uint32_t uid;       /* low 32 bits of the chip's factory MAC               */
} trio_result;

/* Platform side, set by the sketch (or the host test). */
typedef struct
{
    uint32_t (*micros)(void);
    void (*fill)(trio_result *r); /* node, chip, cpu_mhz, free_heap, uptime_ms, uid */
    void (*pwm)(uint8_t duty);    /* put the duty on the pin */
} trio_hooks_t;

static trio_hooks_t trio_hooks;
static uint32_t trio_tasks_done;

/* The math, free of any platform. Returns the sum; *duty gets the 0..255 mapping. */
static float trio_wave(uint16_t step, uint16_t terms, uint8_t *duty)
{
    const float two_pi = 6.28318530718f, half_pi = 1.57079632679f;
    float x = two_pi * (float)(step % TRIO_PERIOD) / (float)TRIO_PERIOD, sum = 0.0f, d;
    uint16_t k;
    for (k = 1; k <= terms; k++)
        sum += sinf((float)k * x) / (float)k;
    d = (sum + half_pi) / 3.14159265359f * 255.0f; /* sum is about -pi/2 .. +pi/2 */
    *duty = d < 0.0f ? 0 : d > 255.0f ? 255 : (uint8_t)(d + 0.5f);
    return sum;
}

static void trio_put_result(muster_wbuf *w, const trio_result *r)
{
    muster_put_u8(w, r->node);
    muster_put_u8(w, r->chip);
    muster_put_u8(w, r->duty);
    muster_put_u16(w, r->cpu_mhz);
    muster_put_f32(w, r->value);
    muster_put_u32(w, r->busy_us);
    muster_put_u32(w, r->tasks);
    muster_put_u32(w, r->free_heap);
    muster_put_u32(w, r->uptime_ms);
    muster_put_u32(w, r->uid);
}

static int trio_get_result(const uint8_t *b, uint16_t len, trio_result *r)
{
    muster_rbuf rb;
    muster_r_init(&rb, b, len);
    r->node = muster_get_u8(&rb);
    r->chip = muster_get_u8(&rb);
    r->duty = muster_get_u8(&rb);
    r->cpu_mhz = muster_get_u16(&rb);
    r->value = muster_get_f32(&rb);
    r->busy_us = muster_get_u32(&rb);
    r->tasks = muster_get_u32(&rb);
    r->free_heap = muster_get_u32(&rb);
    r->uptime_ms = muster_get_u32(&rb);
    r->uid = muster_get_u32(&rb);
    return rb.err ? -1 : 0;
}

static void trio_encode_args(muster_wbuf *w, uint16_t step, uint16_t terms)
{
    muster_put_u16(w, step);
    muster_put_u16(w, terms);
}

/* The task body: decode (step, terms), do the math, drive the pin, report. */
static int16_t trio_wave_tr(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_rbuf rb;
    muster_wbuf w;
    trio_result r;
    uint16_t step, terms;
    uint32_t t0;
    muster_r_init(&rb, a, len);
    step = muster_get_u16(&rb);
    terms = muster_get_u16(&rb);
    if (rb.err || terms == 0 || terms > 2048)
        return -1;
    memset(&r, 0, sizeof r);
    t0 = trio_hooks.micros();
    r.value = trio_wave(step, terms, &r.duty);
    r.busy_us = trio_hooks.micros() - t0;
    if (trio_hooks.pwm)
        trio_hooks.pwm(r.duty);
    trio_tasks_done++;
    trio_hooks.fill(&r);
    r.tasks = trio_tasks_done;
    muster_w_init(&w, ret, cap);
    trio_put_result(&w, &r);
    return w.err ? -1 : (int16_t)w.n;
}
#endif

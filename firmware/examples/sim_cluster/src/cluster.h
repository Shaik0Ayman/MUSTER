/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* cluster.h - a whole Muster cluster inside one chip, for simulation.
 *
 *   node 0   lead     places calls, assigns the module
 *   node 1,2 workers  hold the task; their node IDs come from a fake chip ID
 *
 * The three nodes talk over the in-memory loopback bus (an 8-byte-frame bus
 * that behaves like CAN), so the real runtime - join, heartbeats, placement,
 * failover, module assignment, ID hashing - runs unchanged. What this is NOT:
 * it does not touch the CAN controller, the transceivers or the wiring. It
 * proves the protocol logic on the target CPU and gives you something to watch;
 * it says nothing about real-bus timing.
 *
 * No Arduino dependency: main.cpp wires it to pins, and test_sim_cluster.c
 * runs the same code on the host.
 */
#ifndef SIM_CLUSTER_H
#define SIM_CLUSTER_H
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "muster.h"
#include "muster_link_loopback.h"

#define SC_NODES 3
enum { SC_T_SQUARE = 1 };   /* task: x -> x*x, held by the workers          */
enum { SC_MOD_BLINK = 0 };  /* module: "this node is the active one"        */

typedef struct sim_cluster sim_cluster;
typedef struct { sim_cluster *c; uint8_t idx; } sc_tag;

struct sim_cluster {
    lb_bus       bus;
    lb_ctx       ctx[SC_NODES];
    muster_link  link[SC_NODES];
    muster_t     node[SC_NODES];
    sc_tag       tag[SC_NODES];
    uint8_t      running[SC_NODES];   /* module running on node i              */
    uint32_t   (*now)(void);
    void       (*log)(const char *line);
    int8_t       call;                /* handle of the call in flight, or -1   */
    uint16_t     arg;
    uint8_t      runner;              /* node ID that ran the last call        */
    uint32_t     next_call, done, failed;
    uint8_t      armed;               /* module requested yet? */
};

static void sc_say(sim_cluster *c, const char *fmt, ...)
{
    char line[120];
    int n = snprintf(line, sizeof line, "[%6lu] ", (unsigned long)c->now());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (c->log) c->log(line);
}

/* "node1 (id 57)": which board has this muster node ID */
static const char *sc_name(const sim_cluster *c, uint8_t id, char *buf, size_t cap)
{
    uint8_t i;
    for (i = 0; i < SC_NODES; i++)
        if (c->node[i].id == id) {
            snprintf(buf, cap, "node%u (id %u)", (unsigned)i, (unsigned)id);
            return buf;
        }
    snprintf(buf, cap, "id %u", (unsigned)id);
    return buf;
}

static int16_t sc_square_tr(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_rbuf r; muster_wbuf w; uint32_t x;
    muster_r_init(&r, a, len);
    x = muster_get_u16(&r);
    if (r.err) return -1;
    muster_w_init(&w, ret, cap);
    muster_put_u32(&w, x * x);
    return w.err ? -1 : (int16_t)w.n;
}

static void sc_on_event(uint8_t ev, uint8_t a, uint8_t b, void *user)
{
    sc_tag *t = (sc_tag *)user;
    sim_cluster *c = t->c;
    char who[32];
    if (ev == MUSTER_EV_IDCLASH) {
        sc_say(c, "node%u: ID %u was taken, moved to %u", (unsigned)t->idx, (unsigned)a, (unsigned)b);
        return;
    }
    if (t->idx != 0) return;                       /* the rest is the lead's view */
    switch (ev) {
    case MUSTER_EV_JOINED:       sc_say(c, "lead: %s joined", sc_name(c, a, who, sizeof who)); break;
    case MUSTER_EV_LOST:         sc_say(c, "lead: %s LOST", sc_name(c, a, who, sizeof who)); break;
    case MUSTER_EV_MODULE_START: sc_say(c, "lead: module blink -> %s", sc_name(c, b, who, sizeof who)); break;
    case MUSTER_EV_MODULE_STOP:  sc_say(c, "lead: module blink taken from %s", sc_name(c, b, who, sizeof who)); break;
    case MUSTER_EV_DONE:         c->runner = b; break;
    default: break;
    }
}

static void sc_init(sim_cluster *c, uint32_t (*now)(void), void (*log)(const char *line))
{
    uint8_t i;
    memset(c, 0, sizeof *c);
    c->now = now; c->log = log; c->call = -1; c->next_call = 2000;
    for (i = 0; i < SC_NODES; i++) {
        lb_attach(&c->bus, &c->ctx[i], &c->link[i], i, 8, 500);   /* 8-byte frames, 500 kbit/s: like CAN */
        c->tag[i].c = c; c->tag[i].idx = i;
    }
    muster_begin(&c->node[0], 0, 1, 2, 512, 240, MUSTER_CAP_FPU, &c->link[0], now);
    muster_set_cost(&c->node[0], SC_T_SQUARE, 50);              /* the lead does not hold it */
    muster_register_module(&c->node[0], SC_MOD_BLINK);
    for (i = 1; i < SC_NODES; i++) {
        muster_begin(&c->node[i], i, 0, 2, 512, 240, MUSTER_CAP_FPU, &c->link[i], now);
        muster_set_uid(&c->node[i], 0xA5000000ul + 0x10001ul * i);   /* a fake chip ID */
        muster_register_task(&c->node[i], SC_T_SQUARE, sc_square_tr, 50);
        muster_register_module(&c->node[i], SC_MOD_BLINK);
    }
    for (i = 0; i < SC_NODES; i++) muster_on_event(&c->node[i], sc_on_event, &c->tag[i]);
}

/* Pull a worker's cable (or put it back). The lead cannot be unplugged. */
static void sc_set_plugged(sim_cluster *c, uint8_t i, int plugged)
{
    if (i == 0 || i >= SC_NODES) return;
    c->bus.ports[i].unplugged = plugged ? 0 : 1;
    sc_say(c, "node%u cable %s", (unsigned)i, plugged ? "plugged in" : "PULLED");
}

/* One pass: poll every node, start/stop modules as told, drive the demo call. */
static void sc_step(sim_cluster *c)
{
    uint8_t i;
    for (i = 0; i < SC_NODES; i++) muster_poll(&c->node[i]);
    /* Ask for the module once the workers have joined. Asked at boot, the lead
       is the only live board, so it would take the module and keep it (a module
       stays where it is: moving it loses its state). */
    if (!c->armed && (int32_t)(c->now() - 1500u) >= 0) {
        c->armed = 1;
        muster_set_module(&c->node[0], SC_MOD_BLINK, 1);        /* run it on exactly one board */
    }
    for (i = 0; i < SC_NODES; i++) {                  /* what the generated muster_loop() does */
        c->running[i] = (uint8_t)(muster_module_want(&c->node[i]) & 1u);
        muster_module_report(&c->node[i], c->running[i], 0);
    }
    if (c->call >= 0) {
        uint8_t out[8]; uint16_t n = 0; char who[32];
        int8_t st = muster_result(&c->node[0], c->call, out, sizeof out, &n);
        if (st == MUSTER_PENDING) return;
        c->call = -1;
        if (st == MUSTER_OK) {
            muster_rbuf r; uint32_t v;
            muster_r_init(&r, out, n); v = muster_get_u32(&r);
            c->done++;
            sc_say(c, "square(%u) = %lu   ran on %s", (unsigned)c->arg, (unsigned long)v,
                   sc_name(c, c->runner, who, sizeof who));
        } else {
            c->failed++;
            sc_say(c, "square(%u) FAILED (%d): %s", (unsigned)c->arg, (int)st,
                   st == MUSTER_ENOHOST ? "no live board holds it" :
                   st == MUSTER_ETIMEOUT ? "the board went silent mid-call" : "see MUSTER_E*");
        }
    } else if ((int32_t)(c->now() - c->next_call) >= 0) {
        uint8_t args[2]; muster_wbuf w;
        c->next_call += 1000;
        c->arg = (uint16_t)(c->arg % 90 + 3);
        muster_w_init(&w, args, sizeof args);
        muster_put_u16(&w, c->arg);
        c->call = muster_call_async(&c->node[0], SC_T_SQUARE, args, w.n);
        if (c->call < 0) { c->failed++; sc_say(c, "square(%u) FAILED (%d)", (unsigned)c->arg, (int)c->call); c->call = -1; }
    }
}

/* LED for node i: off = unplugged, steady = alive, fast blink = running the module */
static int sc_led(const sim_cluster *c, uint8_t i, uint32_t t)
{
    if (i != 0 && c->bus.ports[i].unplugged) return 0;
    return c->running[i] ? (int)((t / 125u) & 1u) : 1;
}

static void sc_status(const sim_cluster *c, char *buf, size_t cap)
{
    uint8_t i; size_t n = 0;
    buf[0] = 0;
    for (i = 0; i < SC_NODES && n < cap; i++) {
        const char *s = (i != 0 && c->bus.ports[i].unplugged) ? "unplugged"
                      : c->running[i] ? "BLINK" : "idle";
        n += (size_t)snprintf(buf + n, cap - n, "%snode%u id=%u %s", i ? " | " : "",
                              (unsigned)i, (unsigned)c->node[i].id, s);
    }
}
#endif

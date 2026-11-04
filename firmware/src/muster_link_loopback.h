/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* muster_link_loopback.h - an in-memory broadcast bus for host testing.
 *
 * Behaves like CAN where it matters: every frame reaches every other node,
 * frames are capped at `mtu` bytes (set 8 to emulate classic CAN), and a
 * node can be "unplugged" to test fault handling.
 *
 * Fault injection (all off by default):
 *   ports[i].unplugged  node i neither sends nor hears anything
 *   ports[i].muted      node i hears the bus but its frames are lost
 *   drop_permille       each delivery is lost with this probability (a
 *                       receiver's buffer overflowing), seeded by `rng`
 *   drop                hook: return 1 to lose a frame for every receiver
 */
#ifndef MUSTER_LINK_LOOPBACK_H
#define MUSTER_LINK_LOOPBACK_H
#include "muster.h"

#define LB_MAX_NODES 8
#define LB_QUEUE     256
#define LB_FRAME     64

typedef struct { uint8_t src, len, data[LB_FRAME]; } lb_frame;

typedef struct {
    lb_frame q[LB_QUEUE];
    uint16_t head, tail;
    uint8_t  unplugged, muted;
    uint8_t  id;
} lb_port;

typedef struct lb_bus {
    lb_port  ports[LB_MAX_NODES];
    uint8_t  count;
    uint32_t frames_sent, frames_dropped;
    uint16_t drop_permille;
    uint32_t rng;
    int    (*drop)(struct lb_bus *bus, uint8_t src, const uint8_t *f, uint16_t len);
} lb_bus;

typedef struct { lb_bus *bus; uint8_t index; } lb_ctx;

/* xorshift32: deterministic, so a failing run can be replayed */
static uint32_t lb_rand(lb_bus *b)
{
    uint32_t x = b->rng ? b->rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return b->rng = x;
}

static int lb_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t len)
{
    lb_ctx *c = (lb_ctx *)l->ctx;
    lb_port *me = &c->bus->ports[c->index];
    uint8_t i;
    (void)dst;                               /* physical bus: everyone hears */
    if (len > l->mtu) return -1;
    if (me->unplugged || me->muted) return 0;  /* lost on the wire          */
    c->bus->frames_sent++;
    if (c->bus->drop && c->bus->drop(c->bus, me->id, f, len)) {
        c->bus->frames_dropped++;
        return 0;
    }
    for (i = 0; i < c->bus->count; i++) {
        lb_port *p = &c->bus->ports[i];
        uint16_t next = (uint16_t)((p->tail + 1u) % LB_QUEUE);
        if (i == c->index || p->unplugged || next == p->head) continue;
        if (c->bus->drop_permille && lb_rand(c->bus) % 1000u < c->bus->drop_permille) {
            c->bus->frames_dropped++;
            continue;
        }
        p->q[p->tail].src = me->id;
        p->q[p->tail].len = (uint8_t)len;
        memcpy(p->q[p->tail].data, f, len);
        p->tail = next;
    }
    return 0;
}

static uint16_t lb_recv(muster_link *l, uint8_t *src, uint8_t *f, uint16_t cap)
{
    lb_ctx *c = (lb_ctx *)l->ctx;
    lb_port *p = &c->bus->ports[c->index];
    lb_frame *fr;
    while (p->head != p->tail) {
        fr = &p->q[p->head];
        p->head = (uint16_t)((p->head + 1u) % LB_QUEUE);
        if (fr->len > cap) continue;         /* too big for the caller: drop */
        *src = fr->src;
        memcpy(f, fr->data, fr->len);
        return fr->len;
    }
    return 0;
}

static void lb_attach(lb_bus *bus, lb_ctx *ctx, muster_link *link,
                      uint8_t node_id, uint16_t mtu, uint16_t kbps)
{
    ctx->bus = bus; ctx->index = bus->count;
    bus->ports[bus->count].id = node_id;
    bus->count++;
    link->mtu = mtu; link->kbps = kbps;
    link->send = lb_send; link->recv = lb_recv; link->ctx = ctx; link->set_id = 0;
}
#endif

/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* muster_link_espnow.h - Muster over ESP-NOW: for ESP32 boards with no CAN
 * transceiver. No wires, no router - the boards just talk on one WiFi channel.
 *
 * Why ESP-NOW: it is a shared broadcast medium, which is exactly what a CAN bus
 * is to the runtime. Every board hears every frame; the frame header already
 * names the destination and the core drops frames meant for someone else.
 * Delivery is best-effort (broadcast frames are not acknowledged), so the
 * runtime's own retries and duplicate cache do the recovering.
 *
 * Packet on the air:  src node ID (1 byte) | Muster frame (<= MUSTER_ESPNOW_MTU)
 * The source ID travels in the packet because ESP-NOW only gives the MAC.
 *
 * All boards must use the same channel. A lead that also runs a WiFi access
 * point (for a dashboard) puts the AP on that same channel, and the radio
 * stays there.
 *
 * Threading: the receive callback runs in the WiFi task, not in muster_poll().
 * It only pushes into a single-producer / single-consumer ring (barriers, no
 * locks); muster_poll() pops. The core stays single-threaded.
 *
 * The ring and packet code at the top is plain C and is tested on the host
 * (test_espnow_link.c). The radio glue below it compiles only for ESP32
 * targets and is NOT verified on hardware.
 */
#ifndef MUSTER_LINK_ESPNOW_H
#define MUSTER_LINK_ESPNOW_H
#include "muster.h"

#ifndef MUSTER_ESPNOW_MTU
#define MUSTER_ESPNOW_MTU 64 /* Muster frame bytes; <= MUSTER_MAX_FRAME, packet = 1 + this */
#endif
#ifndef MUSTER_ESPNOW_RING
#define MUSTER_ESPNOW_RING 16 /* received frames waiting for muster_poll(); power of 2 */
#endif

typedef struct
{
    uint8_t src, len;
    uint8_t f[MUSTER_ESPNOW_MTU];
} mespnow_slot;

typedef struct
{
    mespnow_slot slot[MUSTER_ESPNOW_RING];
    volatile uint8_t head, tail; /* head: next write (callback), tail: next read (poll) */
    volatile uint8_t self;       /* this board's node ID, goes out as the first byte    */
    volatile uint32_t dropped;   /* frames lost to a full ring                          */
    uint32_t sent, send_failed;  /* counters for the dashboard / debugging              */
} mespnow_ctx;

#define M_ESPNOW_BARRIER() __sync_synchronize()

/* Build the packet for one frame. Returns its length, or 0 if it does not fit. */
static inline uint16_t mespnow_pack(const mespnow_ctx *c, const uint8_t *f, uint16_t n, uint8_t *out)
{
    uint16_t i;
    if (n == 0 || n > MUSTER_ESPNOW_MTU)
        return 0;
    out[0] = c->self;
    for (i = 0; i < n; i++)
        out[1 + i] = f[i];
    return (uint16_t)(n + 1u);
}

/* Receive side (callback context): validate a packet and queue it. Frames from
 * ourselves and malformed packets are ignored; a full ring drops the frame. */
static inline void mespnow_push(mespnow_ctx *c, const uint8_t *pkt, int n)
{
    uint8_t head = c->head, next = (uint8_t)((head + 1u) % MUSTER_ESPNOW_RING), i;
    mespnow_slot *s;
    if (n < 2 || n > (int)MUSTER_ESPNOW_MTU + 1 || pkt[0] == c->self)
        return;
    if (next == c->tail)
    {
        c->dropped++;
        return;
    }
    s = &c->slot[head];
    s->src = pkt[0];
    s->len = (uint8_t)(n - 1);
    for (i = 0; i < s->len; i++)
        s->f[i] = pkt[1 + i];
    M_ESPNOW_BARRIER(); /* the slot must be complete before the consumer can see it */
    c->head = next;
}

static inline uint16_t mespnow_pop(mespnow_ctx *c, uint8_t *src, uint8_t *f, uint16_t cap)
{
    uint8_t tail = c->tail, i;
    const mespnow_slot *s;
    if (tail == c->head)
        return 0;
    M_ESPNOW_BARRIER();
    s = &c->slot[tail];
    if (s->len > cap)
    {
        c->tail = (uint8_t)((tail + 1u) % MUSTER_ESPNOW_RING);
        return 0;
    }
    *src = s->src;
    for (i = 0; i < s->len; i++)
        f[i] = s->f[i];
    i = s->len;
    M_ESPNOW_BARRIER(); /* finished reading before the producer may reuse the slot */
    c->tail = (uint8_t)((tail + 1u) % MUSTER_ESPNOW_RING);
    return i;
}

/* The core calls this if the node ID changes (muster_set_uid, ID clash). */
static void mespnow_set_id(muster_link *l, uint8_t id) { ((mespnow_ctx *)l->ctx)->self = id; }

static uint16_t mespnow_recv(muster_link *l, uint8_t *src, uint8_t *f, uint16_t cap)
{
    return mespnow_pop((mespnow_ctx *)l->ctx, src, f, cap);
}

/* ------------------------------------------------------------ radio glue */
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static mespnow_ctx *g_mespnow; /* the callback has no user pointer; one link per board */

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void mespnow_on_recv(const esp_now_recv_info_t *, const uint8_t *data, int len)
#else
static void mespnow_on_recv(const uint8_t *, const uint8_t *data, int len)
#endif
{
    if (g_mespnow)
        mespnow_push(g_mespnow, data, len);
}

static int mespnow_send(muster_link *l, uint8_t, const uint8_t *f, uint16_t n)
{
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    mespnow_ctx *c = (mespnow_ctx *)l->ctx;
    uint8_t pkt[MUSTER_ESPNOW_MTU + 1];
    uint16_t len = mespnow_pack(c, f, n, pkt);
    if (!len)
        return -1;
    if (esp_now_send(bcast, pkt, len) != ESP_OK)
    {
        c->send_failed++;
        return -1; /* the runtime retries */
    }
    c->sent++;
    return 0;
}

/* Start the radio and fill in `link`.
 *   ap_running: 1 if the sketch already started a softAP on `channel` (the lead);
 *               0 for a plain station that only needs the channel set (workers).
 * Returns 0 on success. */
static int muster_espnow_begin(muster_link *link, mespnow_ctx *ctx, uint8_t node_id,
                               uint8_t channel, int ap_running)
{
    esp_now_peer_info_t peer;
    memset(ctx, 0, sizeof *ctx);
    ctx->self = node_id;
    if (!ap_running)
    {
        WiFi.persistent(false); /* never write credentials to flash, never auto-connect */
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
    WiFi.setSleep(false); /* modem sleep adds tens of ms of receive latency */
    if (esp_now_init() != ESP_OK)
        return -1;
    g_mespnow = ctx;
    esp_now_register_recv_cb(mespnow_on_recv);
    memset(&peer, 0, sizeof peer);
    memset(peer.peer_addr, 0xFF, sizeof peer.peer_addr);
    peer.channel = 0; /* 0 = whatever channel the radio is on */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK)
        return -2;
    link->mtu = MUSTER_ESPNOW_MTU;
    link->kbps = 1000; /* broadcast ESP-NOW runs at the 1 Mbit/s base rate */
    link->send = mespnow_send;
    link->recv = mespnow_recv;
    link->ctx = ctx;
    link->set_id = mespnow_set_id;
    return 0;
}
#endif /* ESP32 radio glue */

/* Fill in `link` around a ready-made context: used by host tests, where the
 * "radio" is whatever the test wires to mespnow_push() / a send hook. */
static inline void mespnow_attach_raw(muster_link *link, mespnow_ctx *ctx, uint8_t node_id,
                                      int (*send)(muster_link *, uint8_t, const uint8_t *, uint16_t))
{
    memset(ctx, 0, sizeof *ctx);
    ctx->self = node_id;
    link->mtu = MUSTER_ESPNOW_MTU;
    link->kbps = 1000;
    link->send = send;
    link->recv = mespnow_recv;
    link->ctx = ctx;
    link->set_id = mespnow_set_id;
}
#endif

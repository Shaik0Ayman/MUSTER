/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* muster_link_uart.h - a point-to-point link over any byte stream (a UART, a
 * SoftwareSerial, a pipe). A simple, transport-agnostic way to test Muster
 * between boards that have no CAN, for example in a simulator.
 *
 * Wire format of one frame:  0x7E | len | payload[len] | crc16 lo | crc16 hi
 * crc16 = muster_crc16 over len + payload. A bad CRC or a bad length drops the
 * frame and waits for the next 0x7E, so a corrupted byte costs one frame (the
 * runtime's retries recover it). There is no byte stuffing: a 0x7E inside a
 * payload is harmless in sync, and only matters after an error.
 *
 * Point to point: give each cable its own link and node `peer`. A lead with two
 * workers attaches two links (set MUSTER_MAX_LINKS 2 before including muster.h).
 * Not verified on hardware; host tests only.
 */
#ifndef MUSTER_LINK_UART_H
#define MUSTER_LINK_UART_H
#include "muster.h"

#ifndef MUSTER_UART_MTU
#define MUSTER_UART_MTU 32 /* <= MUSTER_MAX_FRAME */
#endif
#define MUSTER_UART_SYNC 0x7Eu

typedef struct
{
    void *io;                                                 /* passed to write/read      */
    void (*write)(void *io, const uint8_t *p, uint16_t n);    /* send n bytes              */
    int (*read)(void *io);                                    /* next byte, or -1 if none  */
    uint8_t peer;                                             /* node ID at the far end    */
    uint8_t state, len, got;
    uint8_t buf[MUSTER_UART_MTU + 2]; /* payload, then the two CRC bytes */
} muster_uart_ctx;

static int muster_uart_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t n)
{
    muster_uart_ctx *c = (muster_uart_ctx *)l->ctx;
    uint8_t out[MUSTER_UART_MTU + 4], hdr;
    uint16_t crc, i;
    (void)dst; /* point to point: the frame header names the destination */
    if (n == 0 || n > MUSTER_UART_MTU)
        return -1;
    hdr = (uint8_t)n;
    out[0] = MUSTER_UART_SYNC;
    out[1] = hdr;
    for (i = 0; i < n; i++)
        out[2 + i] = f[i];
    crc = muster_crc16(out + 1, (uint16_t)(n + 1u));
    out[2 + n] = (uint8_t)crc;
    out[3 + n] = (uint8_t)(crc >> 8);
    c->write(c->io, out, (uint16_t)(n + 4u));
    return 0;
}

/* One state machine step per byte; returns the frame length when one completes. */
static uint16_t muster_uart_feed(muster_uart_ctx *c, uint8_t b, uint8_t *frame, uint16_t cap)
{
    switch (c->state)
    {
    case 0: /* hunting for the sync byte */
        if (b == MUSTER_UART_SYNC)
            c->state = 1;
        return 0;
    case 1: /* length */
        if (b == 0 || b > MUSTER_UART_MTU)
        {
            c->state = (b == MUSTER_UART_SYNC) ? 1 : 0;
            return 0;
        }
        c->len = b;
        c->got = 0;
        c->state = 2;
        return 0;
    default: /* 2: payload, then 2 CRC bytes */
        c->buf[c->got++] = b;
        if (c->got < (uint8_t)(c->len + 2u))
            return 0;
        {
            uint8_t chk[MUSTER_UART_MTU + 1], i;
            uint16_t want, crc;
            chk[0] = c->len;
            for (i = 0; i < c->len; i++)
                chk[1 + i] = c->buf[i];
            crc = muster_crc16(chk, (uint16_t)(c->len + 1u));
            want = (uint16_t)(c->buf[c->len] | ((uint16_t)c->buf[c->len + 1] << 8));
            c->state = 0;
            if (crc != want || c->len > cap)
                return 0;
            for (i = 0; i < c->len; i++)
                frame[i] = c->buf[i];
            return c->len;
        }
    }
}

static uint16_t muster_uart_recv(muster_link *l, uint8_t *src, uint8_t *f, uint16_t cap)
{
    muster_uart_ctx *c = (muster_uart_ctx *)l->ctx;
    int b;
    while ((b = c->read(c->io)) >= 0)
    {
        uint16_t n = muster_uart_feed(c, (uint8_t)b, f, cap);
        if (n)
        {
            *src = c->peer;
            return n;
        }
    }
    return 0; /* nothing complete yet; never blocks */
}

/* kbps: the line rate / 1000 (placement prices transfers by it). */
static inline void muster_uart_attach(muster_link *link, muster_uart_ctx *ctx, void *io,
                                      void (*write)(void *, const uint8_t *, uint16_t),
                                      int (*read)(void *), uint8_t peer, uint16_t kbps)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->io = io;
    ctx->write = write;
    ctx->read = read;
    ctx->peer = peer;
    link->mtu = MUSTER_UART_MTU;
    link->kbps = kbps;
    link->send = muster_uart_send;
    link->recv = muster_uart_recv;
    link->ctx = ctx;
    link->set_id = 0;
}
#endif

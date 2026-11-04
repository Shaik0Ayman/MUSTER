/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* muster_link_spi.h - point-to-point SPI link between two ESP32-S3 boards.
 *
 * Spec: docs/SPI_LINK_SPEC.md. Read it before changing this file.
 *
 * STATUS
 *   [done]  packet framing + CRC-16/CCITT-FALSE        (host-tested)
 *   [done]  queues, master/slave send/recv logic       (host-tested)
 *   [done]  in-memory wire with bit-flip injection     (host-tested)
 *   [done]  ESP32-S3 master/slave drivers (ESP-IDF 4.4 spi_master/spi_slave),
 *           compiled for Arduino-ESP32 2.0.17. NOT YET VERIFIED ON HARDWARE.
 *
 * Packet (fixed size MSPI_PKT bytes, full duplex):
 *   [0xA5][len][payload: len bytes][pad to MSPI_MAX_PAYLOAD][crc16 LE]
 *   CRC covers len + payload. len == 0 is a poll.
 *
 * The portable part never touches hardware: a master gets one function that
 * runs a full-duplex transaction (`xfer`) and one that reads ATTN (`attn`);
 * a slave driver calls mspi_slave_done() after each transaction and clocks
 * out `slave_pkt` in the next one.
 */
#ifndef MUSTER_LINK_SPI_H
#define MUSTER_LINK_SPI_H
#include "muster.h"

#ifndef MSPI_MAX_PAYLOAD
#define MSPI_MAX_PAYLOAD 60u             /* = link MTU (one Muster frame)     */
#endif
#define MSPI_PKT   (MSPI_MAX_PAYLOAD + 4u) /* sync + len + payload + crc16      */
#define MSPI_SYNC  0xA5u
#ifndef MSPI_POLL_MS
#define MSPI_POLL_MS 2u
#endif
#ifndef MSPI_TXQ
#define MSPI_TXQ 4u
#endif
#ifndef MSPI_RXQ
#define MSPI_RXQ 4u
#endif
#if MSPI_MAX_PAYLOAD > 255 || MSPI_MAX_PAYLOAD > MUSTER_MAX_FRAME
#error "MSPI_MAX_PAYLOAD must fit in a byte and in MUSTER_MAX_FRAME"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------- framing (portable) */
static inline uint16_t mspi_crc16(const uint8_t *p, uint16_t n)
{ return muster_crc16(p, n); }                 /* CRC-16/CCITT-FALSE           */

/* Build one fixed-size packet. len may be 0 (poll). Returns 0 or -1. */
static inline int mspi_pack(uint8_t pkt[MSPI_PKT], const uint8_t *payload, uint8_t len)
{
    uint16_t crc;
    if (len > MSPI_MAX_PAYLOAD) return -1;
    memset(pkt, 0, MSPI_PKT);
    pkt[0] = MSPI_SYNC;
    pkt[1] = len;
    if (len) memcpy(pkt + 2, payload, len);
    crc = mspi_crc16(pkt + 1, (uint16_t)(1u + len));
    pkt[MSPI_PKT - 2] = (uint8_t)crc;
    pkt[MSPI_PKT - 1] = (uint8_t)(crc >> 8);
    return 0;
}

/* Validate a received packet. Returns payload length (0 = poll) or -1 if
 * the packet is corrupt and must be dropped. */
static inline int mspi_unpack(const uint8_t pkt[MSPI_PKT], uint8_t *payload, uint8_t cap)
{
    uint8_t len = pkt[1];
    uint16_t got, want;
    if (pkt[0] != MSPI_SYNC || len > MSPI_MAX_PAYLOAD || len > cap) return -1;
    want = mspi_crc16(pkt + 1, (uint16_t)(1u + len));
    got = (uint16_t)(pkt[MSPI_PKT - 2] | ((uint16_t)pkt[MSPI_PKT - 1] << 8));
    if (got != want) return -1;
    if (len) memcpy(payload, pkt + 2, len);
    return len;
}

/* ------------------------------------------------- link state (portable) */
typedef struct { uint8_t len; uint8_t data[MSPI_MAX_PAYLOAD]; } mspi_frame;

typedef struct mspi_ctx mspi_ctx;
struct mspi_ctx {
    uint8_t    peer;                 /* node ID on the other end            */
    uint8_t    is_master;
    mspi_frame txq[MSPI_TXQ]; uint8_t tx_head, tx_count;   /* slave only    */
    mspi_frame rxq[MSPI_RXQ]; uint8_t rx_head, rx_count;
    uint8_t    slave_pkt[MSPI_PKT];  /* slave: packet for the next transaction */
    uint8_t    slave_loaded;         /* slave: slave_pkt carries the txq head */
    uint32_t   last_xfer_ms;
    uint32_t   xfers, crc_errors, tx_drops, rx_drops;   /* telemetry        */
    /* master: run one MSPI_PKT-byte full-duplex transaction; 0 on success */
    int      (*xfer)(mspi_ctx *c, const uint8_t *tx, uint8_t *rx);
    /* master: 1 while the slave asserts ATTN (may be NULL: poll only)     */
    int      (*attn)(mspi_ctx *c);
    /* slave: drive ATTN, 1 = asserted (may be NULL)                       */
    void     (*set_attn)(mspi_ctx *c, int on);
    uint32_t (*millis)(void);
    void      *hw;                   /* platform driver state               */
};

/* ------------------------------------------------------- queue helpers */
static inline int mspi_q_push(mspi_frame *q, uint8_t depth, uint8_t head, uint8_t *count,
                              const uint8_t *data, uint8_t len)
{
    mspi_frame *f;
    if (*count >= depth || len > MSPI_MAX_PAYLOAD) return -1;
    f = &q[(uint8_t)((head + *count) % depth)];
    f->len = len;
    if (len) memcpy(f->data, data, len);
    (*count)++;
    return 0;
}

static inline void mspi_q_pop(uint8_t depth, uint8_t *head, uint8_t *count)
{
    if (!*count) return;
    *head = (uint8_t)((*head + 1u) % depth);
    (*count)--;
}

/* A packet arrived: keep its frame, count it if corrupt. */
static inline void mspi_take(mspi_ctx *c, const uint8_t *pkt)
{
    uint8_t buf[MSPI_MAX_PAYLOAD];
    int n = mspi_unpack(pkt, buf, sizeof buf);
    if (n < 0) { c->crc_errors++; return; }
    if (n == 0) return;                                  /* poll / idle */
    if (mspi_q_push(c->rxq, MSPI_RXQ, c->rx_head, &c->rx_count, buf, (uint8_t)n) != 0)
        c->rx_drops++;
}

/* ------------------------------------------------------- master side */
static inline int mspi_master_xfer(mspi_ctx *c, const uint8_t *payload, uint8_t len)
{
    uint8_t tx[MSPI_PKT], rx[MSPI_PKT];
    if (mspi_pack(tx, payload, len) != 0) return -1;
    if (c->xfer(c, tx, rx) != 0) return -1;
    c->last_xfer_ms = c->millis();
    c->xfers++;
    mspi_take(c, rx);
    return 0;
}

/* ------------------------------------------------------- slave side */
/* Prepare the packet the slave clocks out next: the head of its TX queue,
 * or a poll. The frame leaves the queue only once a transaction carried it. */
static inline void mspi_slave_load(mspi_ctx *c)
{
    if (c->tx_count) {
        const mspi_frame *f = &c->txq[c->tx_head];
        mspi_pack(c->slave_pkt, f->data, f->len);
        c->slave_loaded = 1;
    } else {
        mspi_pack(c->slave_pkt, 0, 0);
        c->slave_loaded = 0;
    }
}

/* The slave driver calls this after every completed transaction. */
static inline void mspi_slave_done(mspi_ctx *c, const uint8_t *rx_pkt)
{
    if (c->slave_loaded) mspi_q_pop(MSPI_TXQ, &c->tx_head, &c->tx_count);
    c->xfers++;
    c->last_xfer_ms = c->millis();
    mspi_take(c, rx_pkt);
    mspi_slave_load(c);
    if (c->set_attn) c->set_attn(c, c->tx_count > 0);
}

/* ------------------------------------------------------- muster_link */
static inline int mspi_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t len)
{
    mspi_ctx *c = (mspi_ctx *)l->ctx;
    (void)dst;                                /* point to point            */
    if (len > MSPI_MAX_PAYLOAD) return -1;
    if (c->is_master) return mspi_master_xfer(c, f, (uint8_t)len);
    if (mspi_q_push(c->txq, MSPI_TXQ, c->tx_head, &c->tx_count, f, (uint8_t)len) != 0) {
        c->tx_drops++;
        return -1;
    }
    if (c->set_attn) c->set_attn(c, 1);
    return 0;
}

static inline uint16_t mspi_recv(muster_link *l, uint8_t *src, uint8_t *f, uint16_t cap)
{
    mspi_ctx *c = (mspi_ctx *)l->ctx;
    while (1) {
        if (!c->rx_count && c->is_master &&
            ((c->attn && c->attn(c)) || c->millis() - c->last_xfer_ms >= MSPI_POLL_MS))
            mspi_master_xfer(c, 0, 0);               /* one poll per call */
        if (!c->rx_count) return 0;
        {
            const mspi_frame *fr = &c->rxq[c->rx_head];
            uint8_t n = fr->len;
            if (n <= cap) memcpy(f, fr->data, n);
            mspi_q_pop(MSPI_RXQ, &c->rx_head, &c->rx_count);
            if (n > cap) { c->rx_drops++; continue; }
            *src = c->peer;
            return n;
        }
    }
}

/* Set up the portable half. kbps is the SPI clock in kbit/s (4 MHz = 4000).
 * A hardware driver (or the test wire) fills in xfer/attn/set_attn/hw.     */
static inline void mspi_init(muster_link *l, mspi_ctx *c, uint8_t peer, int is_master,
                             uint16_t kbps, uint32_t (*millis)(void))
{
    memset(c, 0, sizeof *c);
    c->peer = peer; c->is_master = (uint8_t)(is_master ? 1 : 0); c->millis = millis;
    mspi_slave_load(c);
    l->mtu = MSPI_MAX_PAYLOAD; l->kbps = kbps;
    l->send = mspi_send; l->recv = mspi_recv; l->ctx = c;
}

/* ------------------------------------------- in-memory wire (host tests) */
/* Connects a master ctx to a slave ctx in one process. Each transaction
 * swaps one packet each way; `flip_permille` corrupts one random bit of a
 * packet with that probability (each direction independently).            */
typedef struct {
    mspi_ctx *master, *slave;
    uint16_t  flip_permille;
    uint32_t  rng, flips;
    uint8_t   unplugged;
} mspi_wire;

static inline uint32_t mspi_wire_rand(mspi_wire *w)
{
    uint32_t x = w->rng ? w->rng : 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return w->rng = x;
}

static inline void mspi_wire_noise(mspi_wire *w, uint8_t *pkt)
{
    if (w->flip_permille && mspi_wire_rand(w) % 1000u < w->flip_permille) {
        uint32_t bit = mspi_wire_rand(w) % (MSPI_PKT * 8u);
        pkt[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        w->flips++;
    }
}

static inline int mspi_wire_xfer(mspi_ctx *c, const uint8_t *tx, uint8_t *rx)
{
    mspi_wire *w = (mspi_wire *)c->hw;
    uint8_t mosi[MSPI_PKT];
    if (w->unplugged) { memset(rx, 0xFF, MSPI_PKT); return 0; }  /* MISO floats high */
    memcpy(mosi, tx, MSPI_PKT);
    memcpy(rx, w->slave->slave_pkt, MSPI_PKT);
    mspi_wire_noise(w, mosi);
    mspi_wire_noise(w, rx);
    mspi_slave_done(w->slave, mosi);
    return 0;
}

static inline int mspi_wire_attn(mspi_ctx *c)
{
    mspi_wire *w = (mspi_wire *)c->hw;
    return !w->unplugged && w->slave->tx_count > 0;
}

static inline void mspi_wire_connect(mspi_wire *w, mspi_ctx *master, mspi_ctx *slave)
{
    w->master = master; w->slave = slave;
    master->xfer = mspi_wire_xfer; master->attn = mspi_wire_attn; master->hw = w;
    slave->hw = w;
}

#ifdef __cplusplus
}
#endif

/* ------------------------------------------- ESP32-S3 drivers (M5) */
/* Written against ESP-IDF 4.4 (bundled with Arduino-ESP32 2.0.17).
 * NOT YET VERIFIED ON HARDWARE. Pins come from boards.yaml (spi + attention_pin).
 * DMA buffers are static and word aligned (no malloc rule). The slave keeps
 * one transaction queued; the board must call muster_poll() more often than
 * MSPI_POLL_MS or the master will clock an empty slave and lose a packet
 * (the CRC catches it and the core retries).                              */
#if defined(ESP_PLATFORM)
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "esp_timer.h"

typedef struct {
    spi_host_device_t       host;
    int                     attn_pin;
    spi_device_handle_t     dev;          /* master */
    spi_slave_transaction_t t;            /* slave: the queued transaction */
    uint8_t                 queued;
    uint8_t tx[MSPI_PKT] __attribute__((aligned(4)));
    uint8_t rx[MSPI_PKT] __attribute__((aligned(4)));
} mspi_esp32;

static inline uint32_t mspi_esp32_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static inline int mspi_esp32_xfer(mspi_ctx *c, const uint8_t *tx, uint8_t *rx)
{
    mspi_esp32 *h = (mspi_esp32 *)c->hw;
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    memcpy(h->tx, tx, MSPI_PKT);
    t.length = MSPI_PKT * 8u; t.tx_buffer = h->tx; t.rx_buffer = h->rx;
    if (spi_device_polling_transmit(h->dev, &t) != ESP_OK) return -1;
    memcpy(rx, h->rx, MSPI_PKT);
    return 0;
}

static inline int mspi_esp32_attn(mspi_ctx *c)
{ return gpio_get_level((gpio_num_t)((mspi_esp32 *)c->hw)->attn_pin) == 0; }  /* active low */

static inline void mspi_esp32_set_attn(mspi_ctx *c, int on)
{ gpio_set_level((gpio_num_t)((mspi_esp32 *)c->hw)->attn_pin, on ? 0 : 1); }

static inline int muster_spi_master_begin(muster_link *l, mspi_ctx *c, mspi_esp32 *h,
                                          uint8_t peer, int sck, int mosi, int miso,
                                          int cs, int attn, uint32_t hz)
{
    spi_bus_config_t bus;
    spi_device_interface_config_t dev;
    gpio_config_t io;
    memset(&bus, 0, sizeof bus); memset(&dev, 0, sizeof dev); memset(&io, 0, sizeof io);
    mspi_init(l, c, peer, 1, (uint16_t)(hz / 1000u), mspi_esp32_ms);
    h->host = SPI2_HOST; h->attn_pin = attn;
    bus.sclk_io_num = sck; bus.mosi_io_num = mosi; bus.miso_io_num = miso;
    bus.quadwp_io_num = -1; bus.quadhd_io_num = -1; bus.max_transfer_sz = MSPI_PKT;
    dev.mode = 0; dev.clock_speed_hz = (int)hz; dev.spics_io_num = cs; dev.queue_size = 1;
    if (spi_bus_initialize(h->host, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return -1;
    if (spi_bus_add_device(h->host, &dev, &h->dev) != ESP_OK) return -1;
    io.pin_bit_mask = 1ull << attn; io.mode = GPIO_MODE_INPUT; io.pull_up_en = GPIO_PULLUP_ENABLE;
    if (gpio_config(&io) != ESP_OK) return -1;
    c->xfer = mspi_esp32_xfer; c->attn = mspi_esp32_attn; c->hw = h;
    return 0;
}

/* Slave: collect a finished transaction (if any) and queue the next one. */
static inline void mspi_esp32_slave_service(mspi_ctx *c)
{
    mspi_esp32 *h = (mspi_esp32 *)c->hw;
    spi_slave_transaction_t *done;
    if (h->queued && spi_slave_get_trans_result(h->host, &done, 0) == ESP_OK) {
        h->queued = 0;
        mspi_slave_done(c, h->rx);
    }
    if (!h->queued) {
        memcpy(h->tx, c->slave_pkt, MSPI_PKT);
        memset(&h->t, 0, sizeof h->t);
        h->t.length = MSPI_PKT * 8u; h->t.tx_buffer = h->tx; h->t.rx_buffer = h->rx;
        if (spi_slave_queue_trans(h->host, &h->t, 0) == ESP_OK) h->queued = 1;
    }
}

static inline int mspi_esp32_slave_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t len)
{
    int rc = mspi_send(l, dst, f, len);
    mspi_esp32_slave_service((mspi_ctx *)l->ctx);
    return rc;
}

static inline uint16_t mspi_esp32_slave_recv(muster_link *l, uint8_t *src, uint8_t *f, uint16_t cap)
{
    mspi_esp32_slave_service((mspi_ctx *)l->ctx);
    return mspi_recv(l, src, f, cap);
}

static inline int muster_spi_slave_begin(muster_link *l, mspi_ctx *c, mspi_esp32 *h,
                                         uint8_t peer, int sck, int mosi, int miso,
                                         int cs, int attn, uint32_t hz)
{
    spi_bus_config_t bus;
    spi_slave_interface_config_t slv;
    gpio_config_t io;
    memset(&bus, 0, sizeof bus); memset(&slv, 0, sizeof slv); memset(&io, 0, sizeof io);
    mspi_init(l, c, peer, 0, (uint16_t)(hz / 1000u), mspi_esp32_ms);
    h->host = SPI2_HOST; h->attn_pin = attn; h->queued = 0;
    bus.sclk_io_num = sck; bus.mosi_io_num = mosi; bus.miso_io_num = miso;
    bus.quadwp_io_num = -1; bus.quadhd_io_num = -1; bus.max_transfer_sz = MSPI_PKT;
    slv.spics_io_num = cs; slv.queue_size = 1; slv.mode = 0;
    io.pin_bit_mask = 1ull << attn; io.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&io) != ESP_OK) return -1;
    gpio_set_level((gpio_num_t)attn, 1);                    /* idle high */
    if (spi_slave_initialize(h->host, &bus, &slv, SPI_DMA_CH_AUTO) != ESP_OK) return -1;
    c->set_attn = mspi_esp32_set_attn; c->hw = h;
    l->send = mspi_esp32_slave_send; l->recv = mspi_esp32_slave_recv;
    mspi_esp32_slave_service(c);                            /* queue the first */
    return 0;
}
#endif /* ESP_PLATFORM */

#endif /* MUSTER_LINK_SPI_H */

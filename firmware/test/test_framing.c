/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_framing.c - reassembly edge cases (ROADMAP M1) and the node table.
 * Hand-built frames are fed straight into a lead's receive path, so each
 * case controls exactly which fragments arrive, from whom, on which link. */
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "rig.h"

/* links that swallow everything we send and never receive anything */
static unsigned sent;
static int sink_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t len)
{ (void)l; (void)dst; (void)f; (void)len; sent++; return 0; }
static uint16_t sink_recv(muster_link *l, uint8_t *src, uint8_t *f, uint16_t cap)
{ (void)l; (void)src; (void)f; (void)cap; return 0; }

static muster_link can = { 8, 500, sink_send, sink_recv, 0, 0 };
static muster_link spi = { 60, 4000, sink_send, sink_recv, 0, 0 };
static muster_t    lead;

static uint8_t last_ev, last_a, last_b;
static void on_event(uint8_t ev, uint8_t a, uint8_t b, void *u)
{ (void)u; last_ev = ev; last_a = a; last_b = b; }

/* a 19-byte (v3) JOIN from `src`, as the worker would build it; uid = src */
static void join_msg(uint8_t *b, uint8_t src)
{
    muster_wbuf w; muster_w_init(&w, b, 19);
    muster_put_u8(&w, M_JOIN); muster_put_u8(&w, src); muster_put_u8(&w, MUSTER_BROADCAST);
    muster_put_u8(&w, MUSTER_PROTO_VERSION); muster_put_u8(&w, 1); muster_put_u8(&w, 0);
    muster_put_u16(&w, 2); muster_put_u16(&w, 16); muster_put_u32(&w, 1ul << 2);
    muster_put_u32(&w, src); muster_put_u8(&w, 0);
}

/* fragment `idx` (0..3) of a JOIN split for an 8-byte MTU (6 payload bytes) */
static void feed_join(uint8_t lk, uint8_t src, uint8_t idx)
{
    uint8_t msg[19], f[8], n;
    join_msg(msg, src);
    n = (uint8_t)(idx < 3 ? 6 : 1);
    f[0] = MUSTER_BROADCAST;
    f[1] = (uint8_t)(idx | (idx == 3 ? F_LAST : 0));
    memcpy(f + 2, msg + idx * 6, n);
    m_rx_frame(&lead, lk, src, f, (uint16_t)(n + 2));
}

static void feed_raw(uint8_t src, uint8_t dst, uint8_t idx, int last)
{
    uint8_t f[8] = {0};
    f[0] = dst; f[1] = (uint8_t)(idx | (last ? F_LAST : 0));
    m_rx_frame(&lead, 0, src, f, sizeof f);
}

static int slots_used(void)
{
    int i, n = 0;
    for (i = 0; i < MUSTER_RX_SLOTS; i++) n += lead.rx[i].used;
    return n;
}

static int known(uint8_t id) { return muster_node_by_id(&lead, id) != 0; }

int main(void)
{
    int i;
    printf("\nframing and node-table tests\n\n");
    muster_begin(&lead, 0, 1, 2, 512, 240, 0, &can, now_ms);
    CHECK(muster_add_link(&lead, &spi) == 1, "second link attached as link 1");
    {
        muster_link huge = { MUSTER_MAX_FRAME + 1, 1000, sink_send, sink_recv, 0, 0 };
        muster_link tiny = { 2, 1000, sink_send, sink_recv, 0, 0 };
        CHECK(muster_add_link(&lead, &huge) == -1 && muster_add_link(&lead, &tiny) == -1,
              "links with MTU > MUSTER_MAX_FRAME or <= 2 are refused");
    }
    muster_on_event(&lead, on_event, 0);

    printf("1. reassembly\n");
    feed_raw(3, 9, 0, 0);
    CHECK(slots_used() == 0, "frame for another node dropped before buffering");

    feed_join(0, 3, 0); feed_join(0, 3, 1);
    CHECK(slots_used() == 1 && !known(3), "partial message holds one slot");
    feed_join(0, 3, 2); feed_join(0, 3, 3);
    CHECK(slots_used() == 0 && known(3), "complete JOIN reassembled from 3 fragments");

    feed_join(0, 4, 0); feed_join(0, 4, 2); feed_join(0, 4, 3);
    CHECK(slots_used() == 0 && !known(4), "fragment gap discards the partial message");

    for (i = 0; i < 20; i++) feed_raw(5, 0, (uint8_t)i, 0);   /* 20 x 6 > MUSTER_MAX_MSG */
    CHECK(slots_used() == 0, "reassembly overflow discards the message and frees the slot");

    feed_join(0, 6, 0);
    g_now += MUSTER_CALL_TIMEOUT_MS + 1;
    muster_poll(&lead);
    CHECK(slots_used() == 0 && !known(6), "stale partial message expires");
    feed_join(0, 6, 1); feed_join(0, 6, 2); feed_join(0, 6, 3);
    CHECK(!known(6), "late fragments of an expired message are ignored");

    feed_join(0, 7, 0); feed_join(0, 8, 0);
    feed_join(0, 7, 1); feed_join(0, 8, 1);
    feed_join(0, 8, 2); feed_join(0, 8, 3); feed_join(0, 7, 2); feed_join(0, 7, 3);
    CHECK(known(7) && known(8), "interleaved fragments from two sources both reassemble");

    feed_join(0, 9, 0); feed_join(1, 9, 0);
    feed_join(1, 9, 1); feed_join(0, 9, 1);
    feed_join(0, 9, 2); feed_join(0, 9, 3); feed_join(1, 9, 2); feed_join(1, 9, 3);
    {
        const muster_node *n = muster_node_by_id(&lead, 9);
        CHECK(n && n->rt.links_seen == 3 && n->rt.route == 1,
              "one source interleaved across two links: both arrive, route = faster link");
    }

    feed_join(0, 10, 0); feed_join(0, 10, 0);
    feed_join(0, 10, 1); feed_join(0, 10, 2); feed_join(0, 10, 3);
    CHECK(known(10), "a new first fragment restarts the source's message");

    for (i = 0; i < MUSTER_RX_SLOTS; i++) feed_join(0, (uint8_t)(20 + i), 0);
    feed_join(0, 30, 0); feed_join(0, 30, 1); feed_join(0, 30, 2); feed_join(0, 30, 3);
    CHECK(!known(30), "with every slot busy, a new message is dropped (sender retries)");
    for (i = 0; i < MUSTER_RX_SLOTS; i++) lead.rx[i].used = 0;

    printf("2. node table\n");
    /* rows so far: 0 (lead), 3, 7, 8, 9, 10 */
    feed_join(0, 11, 0); feed_join(0, 11, 1); feed_join(0, 11, 2); feed_join(0, 11, 3);
    feed_join(0, 12, 0); feed_join(0, 12, 1); feed_join(0, 12, 2); feed_join(0, 12, 3);
    CHECK(muster_alive_count(&lead) == MUSTER_MAX_NODES, "node table full");
    last_ev = 0;
    feed_join(0, 13, 0); feed_join(0, 13, 1); feed_join(0, 13, 2); feed_join(0, 13, 3);
    CHECK(!known(13) && last_ev == MUSTER_EV_REFUSED && last_a == 13 && last_b == MUSTER_ACK_FULL,
          "JOIN to a full table is refused with ACK_FULL and an event");
    ((muster_node *)muster_node_by_id(&lead, 3))->alive = 0;     /* node 3 lost */
    feed_join(0, 13, 0); feed_join(0, 13, 1); feed_join(0, 13, 2); feed_join(0, 13, 3);
    CHECK(known(13) && !known(3), "a lost node's row is recycled for a newcomer");

    printf("3. version check\n");
    {
        uint8_t msg[19], f[8];
        join_msg(msg, 14);
        msg[3] = MUSTER_PROTO_VERSION + 1;
        f[0] = MUSTER_BROADCAST; f[1] = 0; memcpy(f + 2, msg, 6);
        m_rx_frame(&lead, 0, 14, f, 8);
        f[1] = 1; memcpy(f + 2, msg + 6, 6); m_rx_frame(&lead, 0, 14, f, 8);
        f[1] = 2; memcpy(f + 2, msg + 12, 6); m_rx_frame(&lead, 0, 14, f, 8);
        f[1] = 3 | F_LAST; memcpy(f + 2, msg + 18, 1); m_rx_frame(&lead, 0, 14, f, 3);
        CHECK(!known(14) && last_ev == MUSTER_EV_VERSION && last_b == MUSTER_PROTO_VERSION + 1,
              "JOIN with a different protocol version is refused");
    }
    return rig_done();
}

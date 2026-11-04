/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_modules.c - lead-assigned modules and MAC-style node IDs (protocol v3).
 *   node 0   lead     holds module 2 only
 *   node 1-3 workers  hold modules 0 and 1; IDs come from muster_set_uid()
 *
 * settle() plays the part of the generated muster_loop(): each board starts
 * what it is told to run unless its hardware is "missing" (it then declines
 * and reports so), and reports what it runs back through the heartbeat.     */
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_loopback.h"
#include "rig.h"

#define NB 4
static lb_bus      bus;
static lb_ctx      ctx[NB];
static muster_link link_[NB];
static muster_t    node[NB];

static uint8_t hw_missing[NB];          /* modules a board declines to start */
static uint8_t g_refused[NB];           /* sticky, like the generated glue   */
static int     starts, stops, clash_events, refuse_events;
static uint8_t clash_who = 0xFF, clash_old, clash_new;
static int     drop_assign;

static void on_event(uint8_t ev, uint8_t a, uint8_t b, void *user)
{
    int who = (int)(*(const uint8_t *)user);
    (void)a; (void)b;
    if (who == 0 && ev == MUSTER_EV_MODULE_START) starts++;
    if (who == 0 && ev == MUSTER_EV_MODULE_STOP) stops++;
    if (who == 0 && ev == MUSTER_EV_REFUSED && b == MUSTER_ACK_CLASH) refuse_events++;
    if (ev == MUSTER_EV_IDCLASH) { clash_events++; clash_who = (uint8_t)who; clash_old = a; clash_new = b; }
}
static const uint8_t idx[NB] = {0, 1, 2, 3};

static int hook(lb_bus *b, uint8_t src, const uint8_t *f, uint16_t len)
{
    (void)b; (void)src;
    if (len < 5 || (f[1] & 0x7Fu) != 0) return 0;
    if (drop_assign > 0 && f[2] == M_ASSIGN) { drop_assign--; return 1; }
    return 0;
}

static void settle(int ms)
{
    int t, i;
    for (t = 0; t < ms; t++) {
        rig_step(1);
        for (i = 0; i < NB; i++) {
            uint8_t want = muster_module_want(&node[i]);
            g_refused[i] |= (uint8_t)(want & hw_missing[i]);
            muster_module_report(&node[i], (uint8_t)(want & ~g_refused[i]), g_refused[i]);
        }
    }
}

static void boot(const uint32_t uid[3], int first_worker_count)
{
    int i;
    memset(&bus, 0, sizeof bus); memset(node, 0, sizeof node);
    memset(hw_missing, 0, sizeof hw_missing); memset(g_refused, 0, sizeof g_refused);
    starts = stops = clash_events = refuse_events = 0; clash_who = 0xFF; drop_assign = 0;
    rig_count = 0; g_now = 0;
    for (i = 0; i < NB; i++) lb_attach(&bus, &ctx[i], &link_[i], (uint8_t)i, 8, 500);
    bus.drop = hook;
    muster_begin(&node[0], 0, 1, 2, 512, 240, MUSTER_CAP_FPU, &link_[0], now_ms);
    muster_register_module(&node[0], 2);
    muster_set_module(&node[0], 0, 1);
    muster_set_module(&node[0], 1, 2);
    muster_set_module(&node[0], 2, 1);
    muster_on_event(&node[0], on_event, (void *)&idx[0]);
    rig_add(&node[0]);
    for (i = 1; i <= first_worker_count; i++) {
        muster_begin(&node[i], 0, 0, 2, 512, 240, MUSTER_CAP_FPU, &link_[i], now_ms);
        muster_set_uid(&node[i], uid[i - 1]);
        muster_register_module(&node[i], 0);
        muster_register_module(&node[i], 1);
        muster_on_event(&node[i], on_event, (void *)&idx[i]);
        rig_add(&node[i]);
    }
}

static uint8_t want_of(int i)       /* what the lead has assigned to board i */
{
    const muster_node *n = muster_node_by_id(&node[0], node[i].id);
    return n ? n->want : 0xFF;
}
static int holders(uint8_t mod)     /* boards (by the lead's table) assigned `mod` */
{
    int i, n = 0;
    for (i = 0; i < NB; i++) n += (want_of(i) != 0xFF && (want_of(i) >> mod & 1)) ? 1 : 0;
    return n;
}
static int holder_of(uint8_t mod)
{
    int i;
    for (i = 1; i < NB; i++) if (want_of(i) != 0xFF && (want_of(i) >> mod & 1)) return i;
    return -1;
}

int main(void)
{
    uint32_t uid[3] = {0x1001u, 0x2002u, 0x3003u};
    int i, h, ok;

    printf("\nmodule and identity tests - lead + 3 workers, 8-byte frames\n\n");

    printf("1. placement\n");
    boot(uid, 3);
    settle(800);
    CHECK(muster_alive_count(&node[0]) == 4, "lead and three workers joined");
    ok = node[1].id != node[2].id && node[2].id != node[3].id && node[1].id != node[3].id &&
         node[1].id >= 1 && node[1].id <= 199;
    CHECK(ok, "workers took distinct IDs from their uid (1..199)");
    CHECK(holders(0) == 1, "module 0 (count 1) runs on exactly one board");
    CHECK(holders(1) == 2, "module 1 (count 2) runs on exactly two boards");
    CHECK((want_of(0) >> 2 & 1) && muster_module_want(&node[0]) == want_of(0),
          "a module only the lead holds is assigned to the lead and it sees it");
    for (ok = 1, i = 1; i < NB; i++)
        ok &= muster_module_want(&node[i]) == want_of(i);
    CHECK(ok, "every worker agrees with the lead about what it should run");
    CHECK(starts == 4, "one START event per assignment");
    {   /* spread: the two copies of module 1 should not both sit with module 0 */
        int h0 = holder_of(0), both = 0;
        for (i = 1; i < NB; i++) both += (want_of(i) == 3);
        CHECK(h0 > 0 && both <= 1, "assignments are spread over the boards");
    }

    printf("2. a board declines its module (hardware missing)\n");
    h = holder_of(0);
    hw_missing[h] = 1;        /* the board notices at once and reports it declined */
    stops = starts = 0;
    settle(800);
    {
        int h2 = holder_of(0);
        CHECK(h2 > 0 && h2 != h && holders(0) == 1,
              "the lead moved module 0 to a board that did not decline it");
        CHECK(holders(1) == 2, "module 1 is still on two boards");
    }

    printf("3. a board is lost\n");
    h = holder_of(0);
    {
        int before_alive = muster_alive_count(&node[0]);
        bus.ports[h].unplugged = 1;
        settle(MUSTER_NODE_TIMEOUT_MS + 600);
        CHECK(muster_alive_count(&node[0]) == before_alive - 1, "the lead noticed it is gone");
    }
    CHECK(holders(0) == 1 && holder_of(0) != h && want_of(h) != 0xFF &&
          !(want_of(h) & 3), "module 0 was re-placed on a survivor; the dead board holds none");
    CHECK(holders(1) == 2, "module 1 is back at two boards on the survivors");
    bus.ports[h].unplugged = 0;
    g_refused[h] = 0; hw_missing[h] = 0;
    settle(2500);
    CHECK(muster_alive_count(&node[0]) == 4, "the board rejoins");
    CHECK(holders(0) == 1, "and module 0 still runs exactly once (no needless move)");

    printf("4. a lost ASSIGN heals on the next heartbeat\n");
    muster_set_module(&node[0], 1, 3);
    drop_assign = 4;
    settle(1200);
    CHECK(drop_assign == 0, "(the bus did drop ASSIGN frames)");
    CHECK(holders(1) == 3, "module 1 now runs on three boards");
    for (ok = 1, i = 1; i < NB; i++)
        ok &= muster_module_want(&node[i]) == want_of(i);
    CHECK(ok, "every worker eventually holds exactly the set the lead assigned");

    printf("5. two boards with the same ID\n");
    {   /* find a second uid whose first hash equals the first's */
        uint32_t a = 0x5005u, b = a + 1;
        while (m_uid_id(b, 0) != m_uid_id(a, 0)) b++;
        uid[0] = a; uid[1] = b; uid[2] = 0x7007u;
        boot(uid, 3);
        CHECK(node[1].id == node[2].id, "(setup) boards 1 and 2 hashed to the same ID");
        settle(1500);
        CHECK(clash_events == 1 && refuse_events >= 1,
              "exactly one board was told to move (the lead refused the later JOIN)");
        CHECK(muster_alive_count(&node[0]) == 4, "all four boards are in the lead's table");
        CHECK(node[1].id != node[2].id, "they now hold different IDs");
        CHECK(clash_who != 0xFF && node[clash_who].id == clash_new && clash_new != clash_old,
              "the mover re-hashed to a new ID");
        CHECK(muster_node_by_id(&node[0], node[1].id) && muster_node_by_id(&node[0], node[2].id) &&
              muster_node_by_id(&node[0], node[1].id)->uid != muster_node_by_id(&node[0], node[2].id)->uid,
              "the table keeps both, told apart by uid");
        CHECK(holders(0) == 1 && holders(1) == 2, "modules are placed across all of them");
    }
    printf("6. an ACK meant for another board is ignored\n");
    {   /* two unjoined boards share an ID; the lead's answers are for one uid */
        muster_t probe;
        uint8_t b[16]; muster_wbuf w; uint8_t id0;
        muster_begin(&probe, 0, 0, 2, 512, 240, 0, &link_[3], now_ms);
        muster_set_uid(&probe, 0x9009u);
        id0 = probe.id;
#define ACK_TO(code, uid) do { muster_w_init(&w, b, sizeof b); \
            muster_put_u8(&w, M_ACK); muster_put_u8(&w, 0); muster_put_u8(&w, probe.id); \
            muster_put_u8(&w, MUSTER_PROTO_VERSION); muster_put_u8(&w, code); \
            muster_put_u32(&w, uid); m_handle(&probe, 0, b, w.n); } while (0)
        ACK_TO(MUSTER_ACK_OK, 0x1234u);
        CHECK(!probe.joined, "an OK for a different uid does not make it join");
        ACK_TO(MUSTER_ACK_CLASH, 0x1234u);
        CHECK(probe.id == id0 && probe.salt == 0, "a CLASH for a different uid does not move it");
        ACK_TO(MUSTER_ACK_CLASH, 0x9009u);
        CHECK(probe.id != id0 && probe.salt == 1, "a CLASH for its own uid does");
        ACK_TO(MUSTER_ACK_OK, 0x9009u);
        CHECK(probe.joined, "an OK for its own uid joins");
#undef ACK_TO
    }

    return rig_done();
}

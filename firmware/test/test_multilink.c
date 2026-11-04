/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_multilink.c - ROADMAP M2, the tests listed in docs/MULTILINK_SPEC.md.
 *   bus A: MTU 8,  500 kbps  - lead 0, worker 1, worker 2   ("CAN")
 *   bus B: MTU 64, 4000 kbps - lead 0, worker 1             ("SPI")
 * Workers 1 and 2 are identical apart from worker 1's second link.
 * (Spec test 1, "single-link tests untouched", is test_bus.c, which runs
 *  unchanged; spec test 7, the footprint, is `make avr`.)                 */
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_loopback.h"
#include "rig.h"

enum { T_SUM = 1 };

static lb_bus      bus_a, bus_b;
static lb_ctx      ctx_a[3], ctx_b[2];
static muster_link a[3], b[2];
static muster_t    node[3];
static int         lost, accepted2;
static uint8_t     last_placed = 0xFF;

static int16_t tr_sum(const uint8_t *in, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_wbuf w; uint32_t s = 0; uint16_t i;
    for (i = 0; i < len; i++) s += in[i];
    muster_w_init(&w, ret, cap); muster_put_u32(&w, s);
    return w.err ? -1 : (int16_t)w.n;
}

static void on_lead(uint8_t ev, uint8_t x, uint8_t y, void *u)
{
    (void)x; (void)u;
    if (ev == MUSTER_EV_LOST) lost++;
    if (ev == MUSTER_EV_PLACED) last_placed = y;
}
static void on_w2(uint8_t ev, uint8_t x, uint8_t y, void *u)
{ (void)x; (void)y; (void)u; if (ev == MUSTER_EV_ACCEPTED) accepted2++; }

static int big_call(uint8_t *placed_on)
{
    uint8_t args[48], out[8]; uint16_t n = 0, i; uint32_t want = 0;
    muster_rbuf r; int8_t st;
    for (i = 0; i < sizeof args; i++) { args[i] = (uint8_t)(i * 7); want += args[i]; }
    st = rig_wait(&node[0], muster_call_async(&node[0], T_SUM, args, sizeof args),
                  out, sizeof out, &n, 3000);
    if (placed_on) *placed_on = last_placed;
    muster_r_init(&r, out, n);
    return st == MUSTER_OK && muster_get_u32(&r) == want && !r.err;
}

static const muster_route *lead_route_to(uint8_t id)
{ return &muster_node_by_id(&node[0], id)->rt; }

int main(void)
{
    int i; uint8_t placed;
    printf("\nmulti-link tests - CAN-like bus A + SPI-like bus B\n\n");
    for (i = 0; i < 3; i++) lb_attach(&bus_a, &ctx_a[i], &a[i], (uint8_t)i, 8, 500);
    for (i = 0; i < 2; i++) lb_attach(&bus_b, &ctx_b[i], &b[i], (uint8_t)i, 64, 4000);
    for (i = 0; i < 3; i++)
        muster_begin(&node[i], (uint8_t)i, i == 0, 2, 512, 240, MUSTER_CAP_FPU, &a[i], now_ms);
    muster_add_link(&node[0], &b[0]);
    muster_add_link(&node[1], &b[1]);
    muster_register_task(&node[1], T_SUM, tr_sum, 100);
    muster_register_task(&node[2], T_SUM, tr_sum, 100);
    muster_set_cost(&node[0], T_SUM, 100);
    muster_on_event(&node[0], on_lead, 0);
    muster_on_event(&node[2], on_w2, 0);
    for (i = 0; i < 3; i++) rig_add(&node[i]);
    rig_step(600);
    CHECK(muster_alive_count(&node[0]) == 3, "all three boards joined");

    printf("2. routes\n");
    CHECK(lead_route_to(1)->links_seen == 3 && lead_route_to(1)->route == 1,
          "lead hears worker 1 on both links and routes to it over bus B");
    CHECK(node[1].lead_rt.links_seen == 3 && node[1].lead_rt.route == 1,
          "worker 1 routes to the lead over bus B too");
    CHECK(lead_route_to(2)->links_seen == 1 && lead_route_to(2)->route == 0,
          "worker 2 is reachable on bus A only");

    printf("3. placement prices the route link\n");
    CHECK(big_call(&placed) && placed == 1,
          "48-byte task placed on worker 1 (fast link) over identical worker 2 (CAN only)");
    {
        unsigned before = (unsigned)bus_a.frames_sent;
        big_call(0);
        CHECK((unsigned)bus_a.frames_sent - before < 3,
              "...and its 48-byte payload did not cross bus A");
    }

    printf("4. unplug bus B\n");
    lost = 0;
    bus_b.ports[0].unplugged = bus_b.ports[1].unplugged = 1;
    CHECK(big_call(0), "call to worker 1 completes over CAN after bus B dies mid-call");
    CHECK(lead_route_to(1)->route == 0,
          "lead switched its route to worker 1 to bus A at the first timeout");
    CHECK(lost == 0 && muster_alive_count(&node[0]) == 3, "no node declared lost");
    rig_step(1500);
    CHECK(lead_route_to(1)->links_seen == 1 && node[1].lead_rt.links_seen == 1 &&
          node[1].lead_rt.route == 0 && lost == 0,
          "both ends forget bus B after a node timeout of silence; still no loss");
    CHECK(big_call(0), "calls keep completing over CAN");

    printf("5. replug bus B\n");
    bus_b.ports[0].unplugged = bus_b.ports[1].unplugged = 0;
    rig_step(500);
    CHECK(lead_route_to(1)->links_seen == 3 && lead_route_to(1)->route == 1 &&
          node[1].lead_rt.route == 1, "route returns to bus B");
    CHECK(big_call(&placed) && placed == 1, "large tasks go back to worker 1");

    printf("6. broadcasts\n");
    rig_step(3000);
    CHECK(node[2].joined && accepted2 == 1 && muster_alive_count(&node[0]) == 3,
          "worker 2 (bus A only) keeps hearing lead broadcasts and stays joined");
    return rig_done();
}

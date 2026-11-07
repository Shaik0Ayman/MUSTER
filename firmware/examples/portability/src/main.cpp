// SPDX-License-Identifier: GPL-3.0-only
// Muster portability self-test: does Muster work on THIS board?
//
// Runs a whole cluster inside one chip: a lead and two workers (one worker on
// small AVRs) on an in-memory 8-byte bus that behaves like CAN. It makes calls,
// unplugs a worker, checks the calls move to the other one, and prints
//     MUSTER SELFTEST: PASS ...      or      MUSTER SELFTEST: FAIL ...
// on Serial (115200) every 10 s. It also compiles the UART link against this
// board's Serial, the transport every Arduino board has.
//
// No wiring, no other boards. Used by CI to check that Muster compiles for
// every board in docs/BOARDS.md; flash it to see it actually run.
#include <Arduino.h>

#ifndef SELFTEST_BOARDS
#if defined(__AVR__) && defined(RAMEND) && (RAMEND < 0x1000)
#define SELFTEST_BOARDS 2                  // 2 KB parts (Uno, Nano): lead + one worker
#else
#define SELFTEST_BOARDS 3
#endif
#endif
#define LB_MAX_NODES SELFTEST_BOARDS
#define LB_QUEUE 12
#define LB_FRAME 8
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_loopback.h"
#include "muster_link_uart.h"

enum { T_SUM = 1 };

static lb_bus      bus;
static lb_ctx      ctx[SELFTEST_BOARDS];
static muster_link lk[SELFTEST_BOARDS];
static muster_t    node[SELFTEST_BOARDS];
static uint16_t    ran_on[SELFTEST_BOARDS];
static int         cur = -1;

// The UART link, attached to this board's Serial just to prove it builds here.
static muster_uart_ctx uart_ctx;
static muster_link     uart_link;
static void uart_write(void *io, const uint8_t *p, uint16_t n) { ((Stream *)io)->write(p, n); }
static int  uart_read(void *io) { return ((Stream *)io)->read(); }

static uint32_t ms(void) { return millis(); }

static int16_t t_sum(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap) {
    muster_wbuf w;
    uint32_t s = 0;
    for (uint16_t i = 0; i < len; i++) s += a[i];
    if (cur >= 0) ran_on[cur]++;
    muster_w_init(&w, ret, cap);
    muster_put_u32(&w, s);
    return w.err ? -1 : (int16_t)w.n;
}

static void poll_all(uint16_t ms_total) {
    uint32_t t0 = millis();
    do {
        for (int i = 0; i < SELFTEST_BOARDS; i++) { cur = i; muster_poll(&node[i]); }
        cur = -1;
    } while (millis() - t0 < ms_total);
}

static uint8_t calls(uint8_t count) {             // returns how many came back right
    uint8_t good = 0;
    for (uint8_t k = 0; k < count; k++) {
        uint8_t args[3] = {k, 2, 3}, out[4];
        uint16_t n = 0;
        int8_t h = muster_call_async(&node[0], T_SUM, args, sizeof args);
        int8_t st = MUSTER_PENDING;
        uint32_t t0 = millis();
        while (h >= 0 && (st = muster_result(&node[0], h, out, sizeof out, &n)) == MUSTER_PENDING &&
               millis() - t0 < 3000)
            poll_all(1);
        muster_rbuf r;
        muster_r_init(&r, out, n);
        if (st == MUSTER_OK && muster_get_u32(&r) == (uint32_t)(k + 5) && !r.err) good++;
    }
    return good;
}

static void run_selftest(void) {
    uint8_t ok1, ok2 = 0, total = 10;
    bool failover = true;
    for (int i = 0; i < SELFTEST_BOARDS; i++) { ran_on[i] = 0; bus.ports[i].unplugged = 0; }
    poll_all(1500);                                   // everyone joins (or rejoins)
    uint8_t alive = muster_alive_count(&node[0]);
    ok1 = calls(total);
#if SELFTEST_BOARDS >= 3
    bus.ports[1].unplugged = 1;                       // pull worker 1's cable
    poll_all(1500);                                   // the lead notices
    ok2 = calls(total);
    failover = ok2 == total && muster_alive_count(&node[0]) == SELFTEST_BOARDS - 1;
    bus.ports[1].unplugged = 0;
#endif
    bool pass = alive == SELFTEST_BOARDS && ok1 == total && failover;
    Serial.print(pass ? F("MUSTER SELFTEST: PASS") : F("MUSTER SELFTEST: FAIL"));
    Serial.print(F("  boards=")); Serial.print(alive); Serial.print('/'); Serial.print(SELFTEST_BOARDS);
    Serial.print(F("  calls=")); Serial.print(ok1); Serial.print('/'); Serial.print(total);
#if SELFTEST_BOARDS >= 3
    Serial.print(F("  after unplug=")); Serial.print(ok2); Serial.print('/'); Serial.print(total);
#endif
    Serial.print(F("  ran on workers:"));
    for (int i = 1; i < SELFTEST_BOARDS; i++) { Serial.print(' '); Serial.print(ran_on[i]); }
    Serial.print(F("  muster_t="));
    Serial.print((unsigned)sizeof(muster_t));
    Serial.println(F(" B"));
}

void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) {}        // native-USB boards: give the port a moment
    for (int i = 0; i < SELFTEST_BOARDS; i++)
        lb_attach(&bus, &ctx[i], &lk[i], (uint8_t)i, 8, 500);
    muster_begin(&node[0], 0, 1, 1, 2, 16, 0, &lk[0], ms);
    muster_set_cost(&node[0], T_SUM, 10);
    for (int i = 1; i < SELFTEST_BOARDS; i++) {
        muster_begin(&node[i], (uint8_t)i, 0, 1, 2, 16, 0, &lk[i], ms);
        muster_register_task(&node[i], T_SUM, t_sum, 10);
    }
    muster_uart_attach(&uart_link, &uart_ctx, &Serial, uart_write, uart_read, 1, 115);
    (void)uart_link;
}

void loop() {
    run_selftest();
    poll_all(10000);
}

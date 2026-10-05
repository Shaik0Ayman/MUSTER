/* test_spi_link.c - ROADMAP M3: the SPI link's queues and master/slave
 * logic, then a two-board runtime over the in-memory SPI wire, then the same
 * with random bit flips. Lead = SPI master, worker = SPI slave, 4 MHz.
 * Retries are raised to 6 so the bit-flip run measures recovery, not luck. */
#define MUSTER_MAX_RETRIES 6u
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_spi.h"
#include "rig.h"

enum { T_SUM = 1 };

static mspi_ctx    mc, sc;
static mspi_wire   wire;
static muster_link ml, sl;
static muster_t    lead, worker;
static unsigned    fragments;           /* master frames with index > 0 */

static int16_t tr_sum(const uint8_t *in, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_wbuf w; uint32_t s = 0; uint16_t i;
    for (i = 0; i < len; i++) s += in[i];
    muster_w_init(&w, ret, cap); muster_put_u32(&w, s); muster_put_u16(&w, len);
    return w.err ? -1 : (int16_t)w.n;
}

/* count fragmented frames the lead sends, then pass to the SPI link */
static int counting_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t len)
{
    if (f[1] & 0x7Fu) fragments++;
    return mspi_send(l, dst, f, len);
}

static int call_sum(uint16_t len)
{
    uint8_t args[MUSTER_MAX_ARGS], out[8]; uint16_t n = 0, i; uint32_t want = 0;
    muster_rbuf r; int8_t st;
    for (i = 0; i < len; i++) { args[i] = (uint8_t)(i * 13 + len); want += args[i]; }
    st = rig_wait(&lead, muster_call_async(&lead, T_SUM, args, len), out, sizeof out, &n, 5000);
    muster_r_init(&r, out, n);
    return st == MUSTER_OK && muster_get_u32(&r) == want && muster_get_u16(&r) == len && !r.err;
}

int main(void)
{
    int i, ok;
    printf("\nSPI link tests - in-memory wire, %u-byte packets\n\n", (unsigned)MSPI_PKT);

    printf("1. queues and master/slave logic\n");
    {
        mspi_frame q[MSPI_TXQ]; uint8_t head = 0, count = 0, d[3] = {1, 2, 3};
        for (i = 0; i < (int)MSPI_TXQ; i++) mspi_q_push(q, MSPI_TXQ, head, &count, d, (uint8_t)(i + 1));
        CHECK(mspi_q_push(q, MSPI_TXQ, head, &count, d, 1) == -1, "queue refuses a push when full");
        mspi_q_pop(MSPI_TXQ, &head, &count);
        mspi_q_push(q, MSPI_TXQ, head, &count, d, 3);
        CHECK(count == MSPI_TXQ && q[head].len == 2 && q[(head + MSPI_TXQ - 1) % MSPI_TXQ].len == 3,
              "queue is FIFO and wraps");
    }
    mspi_init(&ml, &mc, 1, 1, 4000, now_ms);
    mspi_init(&sl, &sc, 0, 0, 4000, now_ms);
    mspi_wire_connect(&wire, &mc, &sc);
    {
        uint8_t f[MSPI_MAX_PAYLOAD], g[MSPI_MAX_PAYLOAD], src = 0; uint16_t n;
        memset(f, 0x5A, sizeof f);
        CHECK(ml.send(&ml, 1, f, sizeof f) == 0 && sl.recv(&sl, &src, g, sizeof g) == MSPI_MAX_PAYLOAD &&
              src == 0 && memcmp(f, g, sizeof f) == 0, "master send: one transaction delivers a full-MTU frame");
        CHECK(sl.send(&sl, 0, f, 10) == 0 && mspi_wire_attn(&mc), "slave send raises ATTN");
        n = ml.recv(&ml, &src, g, sizeof g);          /* poll: slave packet was a poll */
        if (!n) n = ml.recv(&ml, &src, g, sizeof g);  /* next poll carries the frame   */
        CHECK(n == 10 && src == 1 && !mspi_wire_attn(&mc), "master polls on ATTN and collects the frame");
        for (i = 0; i < (int)MSPI_TXQ; i++) sl.send(&sl, 0, f, 4);
        CHECK(sl.send(&sl, 0, f, 4) == -1 && sc.tx_drops == 1, "slave TX queue full: send fails, counted");
        {
            int got = 0, k;
            /* the first transaction carries the poll the slave had loaded
             * before these frames were queued; ATTN makes the master go again */
            for (k = 0; k < 20 && (sc.tx_count || mspi_wire_attn(&mc)); k++)
                if (ml.recv(&ml, &src, g, sizeof g)) got++;
            CHECK(got == (int)MSPI_TXQ && sc.tx_count == 0 && k == (int)MSPI_TXQ + 1,
                  "master drains every queued slave frame (one transaction each, plus one)");
        }
        CHECK(ml.send(&ml, 1, f, MSPI_MAX_PAYLOAD + 1) == -1, "frames above the MTU are refused");
    }

    printf("2. two boards over SPI\n");
    mspi_init(&ml, &mc, 1, 1, 4000, now_ms);
    mspi_init(&sl, &sc, 0, 0, 4000, now_ms);
    mspi_wire_connect(&wire, &mc, &sc);
    ml.send = counting_send;
    muster_begin(&lead, 0, 1, 2, 512, 240, MUSTER_CAP_FPU, &ml, now_ms);
    muster_begin(&worker, 1, 0, 2, 512, 240, MUSTER_CAP_FPU, &sl, now_ms);
    muster_register_task(&worker, T_SUM, tr_sum, 50);
    muster_set_cost(&lead, T_SUM, 50);
    rig_add(&lead); rig_add(&worker);
    rig_step(600);
    CHECK(muster_alive_count(&lead) == 2 && worker.joined, "worker joined the lead over SPI");
    fragments = 0;
    CHECK(call_sum(50), "50-byte arguments (above the CAN MTU) round trip correctly");
    CHECK(fragments == 0, "...in a single SPI frame (message <= 58 bytes needs no fragmentation)");
    CHECK(call_sum(MUSTER_MAX_ARGS), "a fragmented 90-byte call also completes");

    printf("3. bit-flip injection\n");
    wire.flip_permille = 100;                       /* 10%% of packets, each way */
    wire.rng = 777;
    for (ok = 0, i = 0; i < 100; i++) { ok += call_sum((uint16_t)(1 + i % 56)); rig_step(2); }
    printf("        %d/100 calls ok, %lu bits flipped, CRC drops: master %lu, slave %lu\n",
           ok, (unsigned long)wire.flips, (unsigned long)mc.crc_errors, (unsigned long)sc.crc_errors);
    CHECK(ok == 100, "every call completed despite corrupted packets");
    CHECK(mc.crc_errors > 0 && sc.crc_errors > 0, "CRC caught corruption in both directions");
    CHECK(worker.joined && muster_alive_count(&lead) == 2, "nobody lost their place");

    printf("4. cable pulled\n");
    wire.flip_permille = 0;
    wire.unplugged = 1;
    rig_step(1500);
    CHECK(muster_alive_count(&lead) == 1 && !worker.joined, "both sides notice the link is gone");
    wire.unplugged = 0;
    rig_step(1000);
    CHECK(muster_alive_count(&lead) == 2 && worker.joined && call_sum(20), "and recover when it returns");
    return rig_done();
}

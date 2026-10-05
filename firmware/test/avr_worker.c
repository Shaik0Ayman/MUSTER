/* Minimal Nano worker: what a real ATmega328P build would link. */
#define MUSTER_IMPLEMENTATION
#include "muster.h"

static uint32_t ticks;
static uint32_t ms(void) { return ticks++; }
static int  tx(muster_link *l, uint8_t d, const uint8_t *f, uint16_t n) { (void)l;(void)d;(void)f;(void)n; return 0; }
static uint16_t rx(muster_link *l, uint8_t *s, uint8_t *f, uint16_t c) { (void)l;(void)s;(void)f;(void)c; return 0; }

static int16_t sensor(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap) {
    muster_rbuf r; muster_wbuf w;
    muster_r_init(&r, a, len); muster_w_init(&w, ret, cap);
    muster_put_i16(&w, (int16_t)(512 + muster_get_u8(&r)));
    return (int16_t)w.n;
}

static muster_t M;
static muster_link L = { 8, 500, tx, rx, 0 };

int main(void) {
    muster_begin(&M, 2, 0, 1, 2, 16, 0, &L, ms);
    muster_register_task(&M, 2, sensor, 20);
    for (;;) muster_poll(&M);
}

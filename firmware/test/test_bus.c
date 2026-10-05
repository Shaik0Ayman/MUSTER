/* test_bus.c - three simulated boards on an 8-byte-MTU bus (classic CAN).
 *   node 0  lead    "ESP32-S3"  240 MHz, FPU, holds tasks 0,1,2
 *   node 1  worker  "ESP32-S3"  240 MHz, FPU, holds tasks 0,1,2
 *   node 2  worker  "Nano"       16 MHz, no FPU, holds task 2 only
 */
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_loopback.h"
#include <stdio.h>
#include <math.h>

enum { T_GAIT = 0, T_IMU = 1, T_SENSOR = 2, T_MISSING = 5 };

/* ---------------------------------------------------------- fake clock */
static uint32_t g_now;
static uint32_t now_ms(void) { return g_now; }

/* ------------------------------------------- tasks + their trampolines */
/* This is what the toolchain will generate from:
 *   MUSTER_TASK(needs=fpu) Vec3 imu_filter(Vec3 accel, Vec3 gyro)       */
static int16_t tr_imu(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_rbuf r; muster_wbuf w; float acc[3], gyr[3]; int i;
    muster_r_init(&r, a, len);
    for (i = 0; i < 3; i++) acc[i] = muster_get_f32(&r);
    for (i = 0; i < 3; i++) gyr[i] = muster_get_f32(&r);
    if (r.err) return -1;
    muster_w_init(&w, ret, cap);
    for (i = 0; i < 3; i++) muster_put_f32(&w, 0.98f * gyr[i] + 0.02f * acc[i]);
    return w.err ? -1 : (int16_t)w.n;
}

static int16_t tr_gait(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_rbuf r; muster_wbuf w; float speed, turn; int leg;
    muster_r_init(&r, a, len);
    speed = muster_get_f32(&r); turn = muster_get_f32(&r);
    if (r.err) return -1;
    muster_w_init(&w, ret, cap);
    for (leg = 0; leg < 4; leg++)
        muster_put_f32(&w, 30.0f * speed * sinf(leg * 1.5708f) + 10.0f * turn);
    return w.err ? -1 : (int16_t)w.n;
}

static int16_t tr_sensor(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap)
{
    muster_rbuf r; muster_wbuf w; uint8_t ch;
    muster_r_init(&r, a, len); ch = muster_get_u8(&r);
    if (r.err) return -1;
    muster_w_init(&w, ret, cap);
    muster_put_i16(&w, (int16_t)(512 + ch * 10));     /* fake analogRead   */
    return (int16_t)w.n;
}

/* ---------------------------------------------------------------- rig */
static lb_bus      bus;
static lb_ctx      ctx[3];
static muster_link link_[3];
static muster_t    node[3];
static uint8_t     last_placed_node = 0xFF;
static int         lost_events = 0, retry_events = 0;

static const char *ev_name(uint8_t e)
{
    static const char *n[] = {"?", "JOINED", "LOST", "PLACED", "DONE", "RETRY",
                              "ACCEPTED", "VERSION"};
    return e < 8 ? n[e] : "?";
}

static void on_event(uint8_t ev, uint8_t a, uint8_t b, void *user)
{
    (void)user;
    if (ev == MUSTER_EV_PLACED) last_placed_node = b;
    if (ev == MUSTER_EV_LOST) lost_events++;
    if (ev == MUSTER_EV_RETRY) retry_events++;
    if (ev == MUSTER_EV_JOINED || ev == MUSTER_EV_LOST || ev == MUSTER_EV_RETRY)
        printf("        [t=%4lu ms] lead event %-8s a=%u b=%u\n",
               (unsigned long)g_now, ev_name(ev), a, b);
}

static void step(int ms)
{
    int i, k;
    for (k = 0; k < ms; k++) {
        for (i = 0; i < 3; i++) muster_poll(&node[i]);
        g_now++;
    }
}

static int wait_result(muster_t *m, int8_t h, uint8_t *out, uint16_t cap, uint16_t *n, int max_ms)
{
    int8_t st = MUSTER_PENDING;
    int t;
    for (t = 0; t < max_ms; t++) {
        st = muster_result(m, h, out, cap, n);
        if (st != MUSTER_PENDING) return st;
        step(1);
    }
    return st;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  PASS  %s\n", msg); \
                              else { printf("  FAIL  %s\n", msg); fails++; } } while (0)

static void imu_args(uint8_t *buf, uint16_t *len)
{
    muster_wbuf w; int i;
    float acc[3] = {0.1f, 0.2f, 9.8f}, gyr[3] = {1.0f, 2.0f, 3.0f};
    muster_w_init(&w, buf, 64);
    for (i = 0; i < 3; i++) muster_put_f32(&w, acc[i]);
    for (i = 0; i < 3; i++) muster_put_f32(&w, gyr[i]);
    *len = w.n;
}

static int imu_ok(const uint8_t *out, uint16_t n)
{
    muster_rbuf r; float x, y, z;
    muster_r_init(&r, out, n);
    x = muster_get_f32(&r); y = muster_get_f32(&r); z = muster_get_f32(&r);
    return !r.err && fabsf(x - 0.982f) < 1e-4f && fabsf(y - 1.964f) < 1e-4f
                  && fabsf(z - 3.136f) < 1e-4f;
}

int main(void)
{
    uint8_t args[64], out[64]; uint16_t alen, n = 0; int8_t h, h2, st;
    int i;

    printf("\nmuster runtime test - 3 boards, 8-byte frames (classic CAN)\n");
    printf("sizeof(muster_t) on this host: %u bytes\n\n", (unsigned)sizeof(muster_t));

    for (i = 0; i < 3; i++) lb_attach(&bus, &ctx[i], &link_[i], (uint8_t)i, 8, 1000);
    muster_begin(&node[0], 0, 1, 2, 512, 240, MUSTER_CAP_FPU, &link_[0], now_ms);
    muster_begin(&node[1], 1, 0, 2, 512, 240, MUSTER_CAP_FPU, &link_[1], now_ms);
    muster_begin(&node[2], 2, 0, 1,   2,  16, 0,              &link_[2], now_ms);
    for (i = 0; i < 2; i++) {
        muster_register_task(&node[i], T_GAIT, tr_gait, 800);
        muster_register_task(&node[i], T_IMU, tr_imu, 300);
        muster_register_task(&node[i], T_SENSOR, tr_sensor, 20);
    }
    muster_register_task(&node[2], T_SENSOR, tr_sensor, 20);
    muster_on_event(&node[0], on_event, 0);

    printf("1. join\n");
    step(600);
    CHECK(muster_alive_count(&node[0]) == 3, "all three boards joined the lead");
    CHECK(node[1].joined && node[2].joined, "both workers accepted by the lead");
    {
        const muster_node *nano = muster_node_by_id(&node[0], 2);
        CHECK(nano && nano->resident == (1ul << T_SENSOR) && nano->clock_mhz == 16,
              "lead learned the Nano's capabilities and resident tasks");
    }

    printf("2. remote call with fragmentation (24-byte args over 8-byte frames)\n");
    imu_args(args, &alen);
    h = muster_call_async(&node[0], T_IMU, args, alen);
    st = (int8_t)wait_result(&node[0], h, out, sizeof out, &n, 200);
    CHECK(st == MUSTER_OK && imu_ok(out, n), "imu_filter result correct after crossing the bus");
    CHECK(last_placed_node == 1, "placed on the idle worker, not the busier lead");

    printf("3. placement shifts under load\n");
    h  = muster_call_async(&node[0], T_GAIT, args, 8);
    {
        uint8_t first = last_placed_node;
        h2 = muster_call_async(&node[0], T_GAIT, args, 8);
        CHECK(first != last_placed_node,
              "second concurrent call went to a different board than the first");
    }
    st = (int8_t)wait_result(&node[0], h, out, sizeof out, &n, 200);
    CHECK(st == MUSTER_OK && n == 16, "first gait call returned 4 floats");
    st = (int8_t)wait_result(&node[0], h2, out, sizeof out, &n, 200);
    CHECK(st == MUSTER_OK && n == 16, "second gait call returned 4 floats");

    printf("4. the Nano asks for a task it cannot run\n");
    h = muster_call_async(&node[2], T_IMU, args, alen);
    CHECK(h >= 0, "Nano submitted imu_filter (it does not hold that task)");
    st = (int8_t)wait_result(&node[2], h, out, sizeof out, &n, 300);
    CHECK(st == MUSTER_OK && imu_ok(out, n), "Nano received the correct result via the lead");

    printf("5. a board fails mid-call\n");
    h = muster_call_async(&node[0], T_IMU, args, alen);
    CHECK(last_placed_node == 1, "call placed on worker 1");
    bus.ports[1].unplugged = 1;                       /* pull its power   */
    st = (int8_t)wait_result(&node[0], h, out, sizeof out, &n, 2000);
    CHECK(retry_events >= 1, "lead detected the missing reply and re-placed the call");
    CHECK(st == MUSTER_OK && imu_ok(out, n), "work completed elsewhere with the correct result");
    step(1200);
    CHECK(lost_events >= 1 && muster_alive_count(&node[0]) == 2, "lead marked worker 1 as lost");

    printf("6. recovery\n");
    bus.ports[1].unplugged = 0;
    step(1500);
    CHECK(muster_alive_count(&node[0]) == 3, "worker 1 rejoined automatically after power returned");

    printf("7. errors\n");
    h = muster_call_async(&node[0], T_MISSING, args, 1);
    st = (int8_t)wait_result(&node[0], h, out, sizeof out, &n, 50);
    CHECK(st == MUSTER_ENOHOST, "task held by no board reports ENOHOST");

    printf("\n%u frames on the bus. %s\n\n", (unsigned)bus.frames_sent,
           fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
    return fails ? 1 : 0;
}

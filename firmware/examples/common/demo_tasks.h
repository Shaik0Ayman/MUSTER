// demo_tasks.h - tasks shared by the example boards.
// In the full toolchain, muster-cli generates these trampolines (ROADMAP M6).
#pragma once
#include "muster.h"

enum { TASK_IMU_FILTER = 1, TASK_SENSOR_READ = 2 };

// imu_filter(Vec3 accel, Vec3 gyro) -> Vec3   (needs FPU; ESP32 boards hold it)
static inline int16_t imu_filter_tr(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap) {
    muster_rbuf r; muster_wbuf w; float acc[3], gyr[3];
    muster_r_init(&r, a, len);
    for (int i = 0; i < 3; i++) acc[i] = muster_get_f32(&r);
    for (int i = 0; i < 3; i++) gyr[i] = muster_get_f32(&r);
    if (r.err) return -1;
    muster_w_init(&w, ret, cap);
    for (int i = 0; i < 3; i++) muster_put_f32(&w, 0.98f * gyr[i] + 0.02f * acc[i]);
    return w.err ? -1 : (int16_t)w.n;
}

// sensor_read(uint8_t channel) -> int16_t   (every board holds it)
static inline int16_t sensor_read_tr(const uint8_t *a, uint16_t len, uint8_t *ret, uint16_t cap) {
    muster_rbuf r; muster_wbuf w;
    muster_r_init(&r, a, len);
    uint8_t ch = muster_get_u8(&r);
    if (r.err) return -1;
    muster_w_init(&w, ret, cap);
    muster_put_i16(&w, (int16_t)analogRead(ch));
    return (int16_t)w.n;
}

static inline uint16_t encode_imu_args(uint8_t *buf, uint16_t cap) {
    const float acc[3] = {0.1f, 0.2f, 9.8f}, gyr[3] = {1.0f, 2.0f, 3.0f};
    muster_wbuf w; muster_w_init(&w, buf, cap);
    for (int i = 0; i < 3; i++) muster_put_f32(&w, acc[i]);
    for (int i = 0; i < 3; i++) muster_put_f32(&w, gyr[i]);
    return w.n;   // expected result: 0.982, 1.964, 3.136
}

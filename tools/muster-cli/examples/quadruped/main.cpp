// One program for the whole robot. Muster splits it across the boards.
#include <Arduino.h>

struct Vec3 { float x, y, z; };
struct Gait { float hip[4], knee[4]; };

MUSTER_TASK(needs = fpu, cost = 800, ram = 24)
Gait gait_planner(float speed, float turn) {
    Gait g = {};
    for (int leg = 0; leg < 4; leg++) {
        float phase = leg * 0.25f;
        g.hip[leg]  = 30.0f * speed * sinf(phase * 6.2832f) + 10.0f * turn;
        g.knee[leg] = 45.0f * speed * cosf(phase * 6.2832f);
    }
    return g;
}

MUSTER_TASK(needs = fpu, cost = 300, ram = 8)
Vec3 imu_filter(Vec3 accel, Vec3 gyro) {
    const float a = 0.98f;
    Vec3 out = { a * gyro.x + (1 - a) * accel.x,
                 a * gyro.y + (1 - a) * accel.y,
                 a * gyro.z + (1 - a) * accel.z };
    return out;
}

MUSTER_TASK(cost = 20, ram = 1)
int sensor_read(int channel) {
    return analogRead(channel);
}

void setup() {}
void loop() {}

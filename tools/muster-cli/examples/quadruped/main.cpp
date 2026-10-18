// One program for the whole robot. Muster splits it across the boards.
//
// Every board is flashed with this same file. `muster plan` adds the generated
// glue (muster_gen.h) after the last task; it gives each task a twin,
// muster_<task>(...), that runs it on whichever board the lead picks.
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
int16_t sensor_read(uint8_t channel) {
    return (int16_t)analogRead(channel);
}

void setup() {
    Serial.begin(115200);
    if (muster_setup() != 0) Serial.println(F("CAN init failed"));
}

void loop() {
    muster_loop();
#if MUSTER_LEAD
    // The lead runs the control loop; the work lands wherever it fits best.
    static uint32_t last = 0;
    if (millis() - last >= 200) {
        last = millis();
        Vec3 accel = {0.1f, 0.2f, 9.8f}, gyro = {1.0f, 2.0f, 3.0f}, att;
        Gait g;
        if (muster_imu_filter(accel, gyro, &att, 300) == MUSTER_OK &&
            muster_gait_planner(0.5f, att.z * 0.01f, &g, 300) == MUSTER_OK) {
            Serial.print(F("hip0 ")); Serial.println(g.hip[0], 3);
        }
    }
#endif
}

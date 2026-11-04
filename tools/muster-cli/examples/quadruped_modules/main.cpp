// One program, one main.cpp, for every board. Muster splits it across them:
//   tasks    single calls, placed on a live board each time they are made
//   modules  long-running jobs the lead hands to boards, and moves if one drops
#include <Arduino.h>
#include "modules/status_led.h"

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

MUSTER_TASK(cost = 20, ram = 1)
int16_t sensor_read(uint8_t channel) {
    return (int16_t)analogRead(channel);
}

// Declared here, defined below the generated glue: the module calls
// muster_gait_planner(), which the glue provides.
MUSTER_MODULE(needs = fpu, ram = 16, count = 1)
int8_t gait_controller(uint8_t event);

void setup() {
    Serial.begin(115200);
    if (muster_setup() != 0) Serial.println(F("CAN init failed"));
}

void loop() {
    muster_loop();          // polls the bus, then starts/runs/stops this board's modules
}

int8_t gait_controller(uint8_t event) {
    static uint32_t last;
    if (event == MUSTER_MOD_START) { last = 0; return 0; }
    if (event == MUSTER_MOD_RUN && millis() - last >= 200) {
        last = millis();
        Gait g;
        if (muster_gait_planner(0.5f, 0.0f, &g, 300) == MUSTER_OK) {
            Serial.print(F("hip0 "));
            Serial.println(g.hip[0], 3);
        }
    }
    return 0;
}

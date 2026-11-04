// A module kept in its own header. It needs nothing from other boards, so it
// can be defined before the generated glue. Muster calls it with
// MUSTER_MOD_START / MUSTER_MOD_RUN / MUSTER_MOD_STOP.
#pragma once
#include <Arduino.h>

MUSTER_MODULE(ram = 1, count = 1)
int8_t status_led(uint8_t event) {
    static uint32_t last;
    static bool on;
    if (event == MUSTER_MOD_START) {
        pinMode(LED_BUILTIN, OUTPUT);
        last = millis();
        return 0;                            // return < 0 to decline: the lead picks another board
    }
    if (event == MUSTER_MOD_RUN && millis() - last >= 500) {
        last = millis();
        on = !on;
        digitalWrite(LED_BUILTIN, on);
    }
    if (event == MUSTER_MOD_STOP) digitalWrite(LED_BUILTIN, LOW);
    return 0;
}

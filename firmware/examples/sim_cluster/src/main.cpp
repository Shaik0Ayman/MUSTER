// Muster cluster-in-a-chip for the Wokwi simulator (one ESP32-S3).
// Three Muster nodes run in this one firmware over the in-memory bus; see
// cluster.h for what that does and does not prove. Wiring is in diagram.json.
#include <Arduino.h>
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "cluster.h"

static const uint8_t LED_PIN[SC_NODES] = {4, 5, 6};   // node 0 (lead), 1, 2
static const uint8_t BTN_PIN[2] = {15, 16};           // pull worker 1 / worker 2's cable

static sim_cluster C;
static uint32_t ms(void) { return millis(); }
static void say(const char *line) { Serial.println(line); }

void setup() {
    Serial.begin(115200);
    for (uint8_t i = 0; i < SC_NODES; i++) pinMode(LED_PIN[i], OUTPUT);
    for (uint8_t i = 0; i < 2; i++) pinMode(BTN_PIN[i], INPUT_PULLUP);
    Serial.println();
    Serial.println("Muster cluster-in-a-chip: lead + 2 workers on a simulated bus");
    Serial.println("LED steady = alive, fast blink = running the module, off = unplugged");
    Serial.println("Buttons: GPIO15 pulls/replugs node1, GPIO16 node2");
    sc_init(&C, ms, say);
}

void loop() {
    sc_step(&C);

    static bool was[2] = {false, false};              // pressed = LOW
    static uint32_t since[2] = {0, 0};
    static bool plugged[2] = {true, true};
    for (uint8_t i = 0; i < 2; i++) {
        bool down = digitalRead(BTN_PIN[i]) == LOW;
        if (down && !was[i] && millis() - since[i] > 50) {   // edge, debounced
            plugged[i] = !plugged[i];
            sc_set_plugged(&C, i + 1, plugged[i]);
            since[i] = millis();
        }
        was[i] = down;
    }

    for (uint8_t i = 0; i < SC_NODES; i++) digitalWrite(LED_PIN[i], sc_led(&C, i, millis()));

    static uint32_t last = 0;
    if (millis() - last >= 3000) {
        last = millis();
        char s[120];
        sc_status(&C, s, sizeof s);
        Serial.print("        status: ");
        Serial.println(s);
    }
    delay(1);
}

// SPDX-License-Identifier: GPL-3.0-only
// node_selftest - run this on EACH board first, before the Muster demo.
// It needs no Muster library and no wiring, and answers three questions:
//
//   1. Is the board alive, and which chip is it?     -> printed at boot
//   2. Does the PWM pin work?                        -> the pin "breathes" (LED fades up and down)
//   3. Can the boards hear each other over ESP-NOW?  -> every 2 s it lists the other boards it hears
//
// Flash the same sketch to all three boards, open each serial monitor at 115200,
// and look for "RADIO OK". Pass = each board hears the other two.
// The ESP32-C6 also starts a WiFi access point "muster-trio-test" (password
// "muster123") so you can check that your phone / PC can join it.
//
// Works on Arduino-ESP32 2.x and 3.x (C6 needs 3.x). Not yet run on hardware.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "radio_cb.h"

#define CHANNEL 1                       // must match TRIO_CHANNEL in the demo
#ifndef PWM_PIN
  #if CONFIG_IDF_TARGET_ESP32C6
    #define PWM_PIN 18                  // C6: put an LED + 330 ohm from GPIO18 to GND, or probe it
  #else
    #define PWM_PIN 2                   // classic ESP32 DevKit: the on-board LED on most boards
  #endif
#endif

static void pwm_begin() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PWM_PIN, 5000, 8);
#else
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PWM_PIN, 0);
#endif
}
static void pwm_write(uint8_t d) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PWM_PIN, d);
#else
    ledcWrite(0, d);
#endif
}

void setup() {
    Serial.begin(115200);
    delay(400);
    uint64_t mac = ESP.getEfuseMac();
    Serial.printf("\n=== Muster node self-test ===\n");
    Serial.printf("chip: %s rev %d, %d core(s), %u MHz\n", ESP.getChipModel(), (int)ESP.getChipRevision(),
                  ESP.getChipCores(), (unsigned)getCpuFrequencyMhz());
    Serial.printf("flash: %u KB, free heap: %u KB\n", (unsigned)(ESP.getFlashChipSize() / 1024), (unsigned)(ESP.getFreeHeap() / 1024));
    Serial.printf("MAC tail (becomes the Muster uid): %08lX\n", (unsigned long)(uint32_t)mac);
    Serial.printf("PWM pin: GPIO%d  (should breathe)\n", PWM_PIN);

    pwm_begin();
    WiFi.persistent(false);
#if CONFIG_IDF_TARGET_ESP32C6
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("muster-trio-test", "muster123", CHANNEL);
    Serial.printf("access point \"muster-trio-test\" up, IP %s\n", WiFi.softAPIP().toString().c_str());
#else
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
#endif
    WiFi.setSleep(false);
    if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW init FAILED"); return; }
    esp_now_register_recv_cb(on_recv);
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, BCAST, 6);
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    if (esp_now_add_peer(&p) != ESP_OK) Serial.println("add broadcast peer FAILED");
    Serial.printf("ESP-NOW up on channel %d; pinging once a second\n\n", CHANNEL);
}

void loop() {
    static uint32_t t_ping = 0, t_say = 0, t_pwm = 0;
    static int duty = 0, dir = 5;
    uint32_t now = millis();

    if (now - t_pwm >= 10) {                       // a full breath takes about 1 s
        t_pwm = now;
        duty += dir;
        if (duty >= 255) { duty = 255; dir = -5; }
        if (duty <= 0)   { duty = 0;   dir = 5; }
        pwm_write((uint8_t)duty);
    }
    if (now - t_ping >= 1000) {
        t_ping = now;
        esp_now_send(BCAST, (const uint8_t *)MAGIC, sizeof MAGIC - 1);
    }
    if (now - t_say >= 2000) {
        t_say = now;
        int heard = 0;
        for (auto &pr : peers) if (pr.used && now - pr.last_ms < 3500) heard++;
        Serial.printf("[%6lus] hear %d other board(s)", (unsigned long)(now / 1000), heard);
        for (auto &pr : peers) {
            if (!pr.used || now - pr.last_ms >= 3500) continue;
            Serial.printf("  | %02X:%02X:%02X:%02X:%02X:%02X rssi %d dBm, %lu pings",
                          pr.mac[0], pr.mac[1], pr.mac[2], pr.mac[3], pr.mac[4], pr.mac[5], pr.rssi,
                          (unsigned long)pr.count);
        }
        Serial.println(heard >= 2 ? "   => RADIO OK" : heard == 1 ? "   (waiting for the third board)" : "   (nobody yet)");
    }
}

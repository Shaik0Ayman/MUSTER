// SPDX-License-Identifier: GPL-3.0-only
// trio_board.h - what differs between the three boards, and the Arduino glue
// shared by both roles: which role, which pin, PWM, and the vital signs a
// board reports with each result.
//
// Role is picked from the chip: the ESP32-C6 is the lead, every other ESP32 is
// a worker. Override with -DTRIO_ROLE_LEAD or -DTRIO_ROLE_WORKER.
// Ran on hardware 2026-11-07 (see README.md, "What is and is not verified").
#pragma once
#include <Arduino.h>
#include "trio_task.h"

#if !defined(TRIO_ROLE_LEAD) && !defined(TRIO_ROLE_WORKER)
  #if CONFIG_IDF_TARGET_ESP32C6
    #define TRIO_ROLE_LEAD
  #else
    #define TRIO_ROLE_WORKER
  #endif
#endif

// ---- settings you may want to change (all overridable with -D) ----
#ifndef TRIO_CHANNEL
#define TRIO_CHANNEL 1                 // WiFi channel shared by the radio link and the lead's AP
#endif
#ifndef TRIO_AP_SSID
#define TRIO_AP_SSID "muster-trio"     // the lead's access point; the dashboard is at http://192.168.4.1/
#endif
#ifndef TRIO_AP_PASS
#define TRIO_AP_PASS "muster123"       // at least 8 characters
#endif
#ifndef TRIO_CALL_MS
#define TRIO_CALL_MS 50                // the lead starts a new call this often (if a slot is free)
#endif

// PWM pin. Classic ESP32 DevKits: GPIO2 is the on-board blue LED on most
// boards, so the output is visible with no wiring. The C6 DevKit has no plain
// LED (its on-board one is an RGB part), so use GPIO18 with an LED + 330 ohm
// to GND, or a scope. Not taken from boards.yaml: this example is hand-written.
#ifndef TRIO_PWM_PIN
  #if CONFIG_IDF_TARGET_ESP32C6
    #define TRIO_PWM_PIN 18
  #else
    #define TRIO_PWM_PIN 2
  #endif
#endif
#define TRIO_PWM_HZ 5000
#define TRIO_PWM_BITS 8

#if CONFIG_IDF_TARGET_ESP32C6
  #define TRIO_CHIP TRIO_CHIP_C6
  #define TRIO_CHIP_NAME "ESP32-C6"
#elif CONFIG_IDF_TARGET_ESP32S3
  #define TRIO_CHIP TRIO_CHIP_S3
  #define TRIO_CHIP_NAME "ESP32-S3"
#elif CONFIG_IDF_TARGET_ESP32
  #define TRIO_CHIP TRIO_CHIP_ESP32
  #define TRIO_CHIP_NAME "ESP32"
#else
  #define TRIO_CHIP TRIO_CHIP_UNKNOWN
  #define TRIO_CHIP_NAME "ESP32?"
#endif

static uint8_t  trio_last_duty;
static uint8_t (*trio_node_getter)(void);

static uint32_t trio_uid(void) { return (uint32_t)ESP.getEfuseMac(); }   // low 32 bits of the factory MAC

static void trio_pwm_begin(void) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(TRIO_PWM_PIN, TRIO_PWM_HZ, TRIO_PWM_BITS);
#else
    ledcSetup(0, TRIO_PWM_HZ, TRIO_PWM_BITS);
    ledcAttachPin(TRIO_PWM_PIN, 0);
#endif
}

static void trio_pwm(uint8_t duty) {
    trio_last_duty = duty;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(TRIO_PWM_PIN, duty);
#else
    ledcWrite(0, duty);
#endif
#if defined(TRIO_RGB_LED) && defined(RGB_BUILTIN)
    neopixelWrite(RGB_BUILTIN, duty / 4, duty / 4, duty / 4);   // opt-in (-DTRIO_RGB_LED): C6 on-board RGB as a dimmer
#endif
}

static uint32_t trio_micros(void) { return micros(); }
static uint32_t trio_ms(void) { return millis(); }

// Vital signs for a result. `node` comes from whoever owns the muster_t.
static void trio_fill(trio_result *r) {
    r->node = trio_node_getter ? trio_node_getter() : 0;
    r->chip = TRIO_CHIP;
    r->cpu_mhz = (uint16_t)getCpuFrequencyMhz();
    r->free_heap = ESP.getFreeHeap();
    r->uptime_ms = millis();
    r->uid = trio_uid();
}

static void trio_board_begin(uint8_t (*node_getter)(void)) {
    trio_node_getter = node_getter;
    trio_pwm_begin();
    trio_pwm(0);
    trio_hooks.micros = trio_micros;
    trio_hooks.fill = trio_fill;
    trio_hooks.pwm = trio_pwm;
}

// SPDX-License-Identifier: GPL-3.0-only
// Muster dashboard example: an ESP32 lead that keeps the trio workers busy and
// reports what is happening.
//
//   lead_serial : writes a telemetry snapshot to USB four times a second.
//                 On your PC: muster dash --serial <port> --open
//   lead_wifi   : the same, and serves the dashboard itself on its own WiFi
//                 access point "muster-dash" (http://192.168.4.1/).
//
// Workers: flash firmware/examples/trio (env worker_esp32) to one or more
// ESP32 DevKits. They hold the `wave` task; this lead holds no tasks itself.
// Compiles; NOT yet run on hardware.
#include <Arduino.h>
#include <WiFi.h>
#define MUSTER_TELEMETRY 1                 // the lead's build only: workers don't need it
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_espnow.h"
#include "../../trio/muster_trio/trio_task.h"   // the task the trio workers hold
#ifdef DASH_ON_BOARD
#include "muster_dashboard.h"
static muster_dashboard dash;
#endif

#define CHANNEL 1                          // must match the workers (TRIO_CHANNEL)
#define IN_FLIGHT 3

static muster_t    M;
static muster_link LINK;
static mespnow_ctx RADIO;
static int8_t      slot[IN_FLIGHT] = {-1, -1, -1};
static uint16_t    step_no;

static uint32_t ms(void) { return millis(); }
static uint32_t us(void) { return micros(); }

void setup() {
    Serial.begin(115200);
#ifdef DASH_ON_BOARD
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);                // AP for the page; STA side carries ESP-NOW
    WiFi.softAP("muster-dash", "muster123", CHANNEL);
    int rc = muster_espnow_begin(&LINK, &RADIO, 0, CHANNEL, /*ap_running=*/1);
#else
    int rc = muster_espnow_begin(&LINK, &RADIO, 0, CHANNEL, /*ap_running=*/0);
#endif
    if (rc != 0) Serial.println("ESP-NOW init FAILED");
    muster_begin(&M, 0, /*is_lead=*/1, ESP.getChipCores(), ESP.getHeapSize() / 1024,
                 getCpuFrequencyMhz(), MUSTER_CAP_FPU | MUSTER_CAP_WIFI, &LINK, ms);
    muster_set_micros(&M, us);             // microsecond latencies
    muster_set_cost(&M, TRIO_T_WAVE, TRIO_COST);
#ifdef DASH_ON_BOARD
    muster_dashboard_begin(&dash, &M, 80);
    Serial.println("dashboard: join WiFi \"muster-dash\" (muster123), open http://192.168.4.1/");
#else
    Serial.println("dashboard: run  muster dash --serial <this port> --open  on your PC");
#endif
}

void loop() {
    static uint32_t last_frame = 0;
    uint8_t out[MUSTER_MAX_RET];
    uint16_t n;
    muster_poll(&M);

    for (auto &h : slot) {                 // keep a few calls in flight so placement has a choice
        if (h >= 0 && muster_result(&M, h, out, sizeof out, &n) != MUSTER_PENDING) h = -1;
        if (h < 0) {
            uint8_t a[4];
            muster_wbuf w;
            muster_w_init(&w, a, sizeof a);
            trio_encode_args(&w, step_no, TRIO_TERMS);
            h = muster_call_async(&M, TRIO_T_WAVE, a, w.n);
            if (h >= 0) step_no++;
        }
    }

    if (millis() - last_frame >= 250) {    // 4 snapshots a second, about 2 KB/s at most
        static uint8_t frame[MUSTER_TEL_FRAME_MAX];
        last_frame = millis();
        uint16_t len = muster_telemetry_frame(&M, frame, sizeof frame);
        if (len) Serial.write(frame, len);
    }
#ifdef DASH_ON_BOARD
    muster_dashboard_poll(&dash);
#endif
}

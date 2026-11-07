// SPDX-License-Identifier: GPL-3.0-only
// radio_cb.h - the ESP-NOW receive side of node_selftest. It lives in a header
// (not the .ino) because Arduino-ESP32 2.x and 3.x give the callback different
// signatures, and the Arduino sketch preprocessor generates prototypes for
// functions on BOTH sides of an #if.
#pragma once
#include <Arduino.h>
#include <esp_now.h>

struct Peer { uint8_t mac[6]; uint32_t last_ms; uint32_t count; int rssi; bool used; };
static Peer peers[6];                   // written by the radio callback, read by loop(): a diagnostic,
                                        // so a torn read is tolerated rather than locked
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const char MAGIC[] = "MSTR-PING";

static void note_peer(const uint8_t *mac, int rssi) {
    Peer *slot = nullptr;
    for (auto &p : peers) {
        if (p.used && memcmp(p.mac, mac, 6) == 0) { slot = &p; break; }
        if (!p.used && !slot) slot = &p;
    }
    if (!slot) return;
    memcpy(slot->mac, mac, 6);
    slot->used = true;
    slot->last_ms = millis();
    slot->count++;
    slot->rssi = rssi;
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len >= (int)sizeof MAGIC - 1 && memcmp(data, MAGIC, sizeof MAGIC - 1) == 0)
        note_peer(info->src_addr, info->rx_ctrl ? info->rx_ctrl->rssi : 0);
}
#else
static void on_recv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len >= (int)sizeof MAGIC - 1 && memcmp(data, MAGIC, sizeof MAGIC - 1) == 0)
        note_peer(mac, 0);              // 2.x does not hand the callback an RSSI
}
#endif


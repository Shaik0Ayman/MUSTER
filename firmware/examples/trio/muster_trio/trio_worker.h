// SPDX-License-Identifier: GPL-3.0-only
// trio_worker.h - the worker role. Holds the `wave` task; has no idea who the
// lead is or which other boards exist. The same image runs on both DevKits:
// each hashes its node ID from its own chip MAC (muster_set_uid).
// NOT yet run on hardware.
#pragma once

static muster_t     M;
static muster_link  LINK;
static mespnow_ctx  RADIO;

static uint8_t worker_id(void) { return M.id; }

void trio_setup(void) {
    Serial.begin(115200);
    delay(300);
    trio_board_begin(worker_id);
    if (muster_espnow_begin(&LINK, &RADIO, 1, TRIO_CHANNEL, /*ap_running=*/0) != 0)
        Serial.println("ESP-NOW init FAILED");
    muster_begin(&M, 1, /*is_lead=*/0, ESP.getChipCores(), ESP.getHeapSize() / 1024,
                 getCpuFrequencyMhz(), MUSTER_CAP_FPU | MUSTER_CAP_WIFI, &LINK, trio_ms);
    muster_set_uid(&M, trio_uid());                       // node ID = hash of the MAC; the link follows
    muster_register_task(&M, TRIO_T_WAVE, trio_wave_tr, TRIO_COST);
    Serial.printf("\nmuster trio WORKER (%s)  uid=%08lX  node id=%u  pwm pin=%d  channel=%d\n",
                  TRIO_CHIP_NAME, (unsigned long)trio_uid(), M.id, TRIO_PWM_PIN, TRIO_CHANNEL);
}

void trio_loop(void) {
    static uint32_t last_print = 0, last_blink = 0;
    uint32_t now = millis();
    muster_poll(&M);
    if (!M.joined && now - last_blink >= 250) {           // not linked to a lead: blink so you can see it
        last_blink = now;
        trio_pwm(trio_last_duty ? 0 : 255);
    }
    if (now - last_print >= 2000) {
        last_print = now;
        Serial.printf("[%7lu] node %3u  %-9s  tasks run=%lu  duty=%3u  rx ring drops=%lu  tx fails=%lu\n",
                      (unsigned long)now, M.id, M.joined ? "joined" : "searching",
                      (unsigned long)trio_tasks_done, trio_last_duty,
                      (unsigned long)RADIO.dropped, (unsigned long)RADIO.send_failed);
    }
}

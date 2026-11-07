// SPDX-License-Identifier: GPL-3.0-only
// muster_trio - three ESP32s share a math task over Muster, on ESP-NOW (no CAN).
//
//   ESP32-C6   -> LEAD   : places the calls, hosts the dashboard (WiFi AP "muster-trio")
//   ESP32 x2   -> WORKER : same image on both; runs the task, drives a PWM pin
//
// One sketch for all three boards - the role follows the chip. Flash it to each
// board as-is. See README.md next to this folder for wiring (none), flashing
// and what to expect. Needs the Muster library (the `firmware/` folder of the repo).
#include <Arduino.h>
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_espnow.h"
#include "trio_board.h"

#ifdef TRIO_ROLE_LEAD
#include "trio_lead.h"
#else
#include "trio_worker.h"
#endif

void setup() { trio_setup(); }
void loop() { trio_loop(); }

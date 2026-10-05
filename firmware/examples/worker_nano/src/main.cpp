// Worker board (Arduino Nano, node 2). Holds only sensor_read, but every 2 s
// asks the bus for imu_filter - a task it cannot run - and prints the answer.
#include <Arduino.h>
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_mcp2515.h"
#include "demo_tasks.h"

static MCP2515        mcp(10);
static muster_mcp_ctx ctx;
static muster_t       M;
static muster_link    CAN;
static uint32_t ms(void) { return millis(); }

static int8_t   pending = -1;
static uint32_t last_ask = 0;

void setup() {
    Serial.begin(115200);
    if (muster_mcp2515_begin(&CAN, &ctx, &mcp, 2) != 0) Serial.println(F("MCP2515 init FAILED"));
    muster_begin(&M, 2, /*is_lead=*/0, 1, 2, 16, 0, &CAN, ms);
    muster_register_task(&M, TASK_SENSOR_READ, sensor_read_tr, 20);
}

void loop() {
    muster_poll(&M);
    if (pending < 0 && millis() - last_ask > 2000 && M.joined) {
        uint8_t args[24];
        uint16_t n = encode_imu_args(args, sizeof args);
        pending = muster_call_async(&M, TASK_IMU_FILTER, args, n);
        last_ask = millis();
    }
    if (pending >= 0) {
        uint8_t out[12]; uint16_t n = 0;
        int8_t st = muster_result(&M, pending, out, sizeof out, &n);
        if (st == MUSTER_PENDING) return;
        pending = -1;
        if (st != MUSTER_OK) { Serial.print(F("imu_filter failed: ")); Serial.println(st); return; }
        muster_rbuf r; muster_r_init(&r, out, n);
        Serial.print(F("imu_filter via bus: "));
        for (int i = 0; i < 3; i++) { Serial.print(muster_get_f32(&r), 3); Serial.print(' '); }
        Serial.println();
    }
}

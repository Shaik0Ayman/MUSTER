// Worker board (ESP32-S3, node 1). Holds imu_filter and sensor_read.
// Build env worker_esp32_spi also attaches an SPI link to the lead (slave side).
#include <Arduino.h>
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_twai.h"
#ifdef MUSTER_DEMO_SPI
#include "muster_link_spi.h"
#endif
#include "demo_tasks.h"

static muster_t    M;
static muster_link CAN;
#ifdef MUSTER_DEMO_SPI
static muster_link SPIL;
static mspi_ctx    spi_ctx;
static mspi_esp32  spi_hw;
#endif
static uint32_t ms(void) { return millis(); }

void setup() {
    Serial.begin(115200);
    // pins: boards.yaml esp32-s3-devkitc-1 can.pins
    if (muster_twai_begin(&CAN, 1, GPIO_NUM_5, GPIO_NUM_4) != 0) Serial.println("TWAI init FAILED");
    muster_begin(&M, 1, /*is_lead=*/0, 2, 512, 240, MUSTER_CAP_FPU, &CAN, ms);
#ifdef MUSTER_DEMO_SPI
    // pins: boards.yaml esp32-s3-devkitc-1 spi + attention_pin
    if (muster_spi_slave_begin(&SPIL, &spi_ctx, &spi_hw, /*peer=*/0,
                               12, 11, 13, 10, 6, 4000000) != 0 ||
        muster_add_link(&M, &SPIL) < 0)
        Serial.println("SPI link init FAILED");
#endif
    muster_register_task(&M, TASK_IMU_FILTER, imu_filter_tr, 300);
    muster_register_task(&M, TASK_SENSOR_READ, sensor_read_tr, 20);
}

// Poll faster than MSPI_POLL_MS so the SPI slave always has a transaction queued.
void loop() { muster_poll(&M); }

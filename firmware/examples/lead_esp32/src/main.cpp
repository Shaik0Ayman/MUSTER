// Lead board (ESP32-S3, node 0). Places tasks, relays results, logs events.
// Build env lead_esp32_spi also attaches an SPI link to worker 1 (master side).
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

static void on_event(uint8_t ev, uint8_t a, uint8_t b, void *) {
    static const char *names[] = {"?", "JOINED", "LOST", "PLACED", "DONE", "RETRY",
                                  "ACCEPTED", "VERSION", "REFUSED"};
    Serial.printf("[%lu] %s a=%u b=%u\n", (unsigned long)millis(), ev < 9 ? names[ev] : "?", a, b);
}

void setup() {
    Serial.begin(115200);
    // pins: boards.yaml esp32-s3-devkitc-1 can.pins
    if (muster_twai_begin(&CAN, 0, GPIO_NUM_5, GPIO_NUM_4) != 0) Serial.println("TWAI init FAILED");
    muster_begin(&M, 0, /*is_lead=*/1, 2, 512, 240, MUSTER_CAP_FPU, &CAN, ms);
#ifdef MUSTER_DEMO_SPI
    // pins: boards.yaml esp32-s3-devkitc-1 spi + attention_pin; 4 MHz to start (SPI_LINK_SPEC.md)
    if (muster_spi_master_begin(&SPIL, &spi_ctx, &spi_hw, /*peer=*/1,
                                12, 11, 13, 10, 6, 4000000) != 0 ||
        muster_add_link(&M, &SPIL) < 0)
        Serial.println("SPI link init FAILED");
#endif
    muster_register_task(&M, TASK_IMU_FILTER, imu_filter_tr, 300);
    muster_register_task(&M, TASK_SENSOR_READ, sensor_read_tr, 20);
    muster_on_event(&M, on_event, nullptr);
    Serial.println("muster lead up");
}

void loop() { muster_poll(&M); }

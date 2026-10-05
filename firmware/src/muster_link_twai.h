/* muster_link_twai.h - CAN link for ESP32 / ESP32-S3 using the on-chip TWAI
 * controller. Needs an external transceiver (SN65HVD230 at 3.3 V).
 *
 * Muster frames use 29-bit extended IDs in a HIGH range. On CAN, lower IDs
 * win arbitration, so the application's frames beat Muster's - as long as it
 * uses extended IDs below 0x1F000000 or standard IDs up to 0x7C0 (see
 * docs/PROTOCOL.md). Frames outside Muster's range are ignored, so Muster can
 * share an app's bus.
 *
 * Status: written against the ESP-IDF 4.4 TWAI driver (driver/twai.h) bundled
 * with Arduino-ESP32 2.0.17; compiles there. Not yet verified on hardware.
 */
#ifndef MUSTER_LINK_TWAI_H
#define MUSTER_LINK_TWAI_H
#include "muster.h"
#include "driver/twai.h"

#ifndef MUSTER_CAN_BASE
#define MUSTER_CAN_BASE 0x1F000000ul   /* low priority; low byte = source node */
#endif
#define MUSTER_CAN_MASK 0x1FFFFF00ul

static int mtwai_send(muster_link *l, uint8_t dst, const uint8_t *f, uint16_t len)
{
    twai_message_t msg;
    uint8_t self = (uint8_t)(uintptr_t)l->ctx;
    (void)dst;                                   /* dst travels in the frame */
    memset(&msg, 0, sizeof msg);
    msg.extd = 1;
    msg.identifier = MUSTER_CAN_BASE | self;
    msg.data_length_code = (uint8_t)len;
    memcpy(msg.data, f, len);
    return twai_transmit(&msg, pdMS_TO_TICKS(5)) == ESP_OK ? 0 : -1;
}

static uint16_t mtwai_recv(muster_link *l, uint8_t *src, uint8_t *f, uint16_t cap)
{
    twai_message_t msg;
    (void)l;
    while (twai_receive(&msg, 0) == ESP_OK) {
        if (!msg.extd || (msg.identifier & MUSTER_CAN_MASK) != MUSTER_CAN_BASE) continue;
        if (msg.data_length_code > cap) continue;
        *src = (uint8_t)(msg.identifier & 0xFFu);
        memcpy(f, msg.data, msg.data_length_code);
        return msg.data_length_code;
    }
    return 0;
}

/* tx/rx: GPIOs to the transceiver, e.g. GPIO_NUM_5 / GPIO_NUM_4 */
static int muster_twai_begin(muster_link *link, uint8_t node_id, int tx, int rx)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)tx, (gpio_num_t)rx,
                                                          TWAI_MODE_NORMAL);
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  a = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    g.rx_queue_len = 32;
    if (twai_driver_install(&g, &t, &a) != ESP_OK || twai_start() != ESP_OK) return -1;
    link->mtu = 8; link->kbps = 500;
    link->send = mtwai_send; link->recv = mtwai_recv;
    link->ctx = (void *)(uintptr_t)node_id;
    return 0;
}
#endif

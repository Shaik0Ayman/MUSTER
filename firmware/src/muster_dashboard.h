/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* muster_dashboard.h - OPTIONAL: let an ESP32 lead serve the Muster dashboard
 * itself, over WiFi. Most setups don't need this: `muster dash --serial PORT`
 * hosts the same page on your PC and the board only writes telemetry frames
 * to USB. Use this header when there is no PC nearby.
 *
 * Cost when you include it: the page (~30 KB of flash), Arduino's WebServer,
 * one MUSTER_TEL_FRAME_MAX buffer, and a few ms in loop() while a browser is
 * fetching. Leave it out and you pay nothing.
 *
 *     #define MUSTER_TELEMETRY 1          // before muster.h, lead build only
 *     #include "muster.h"
 *     #include "muster_dashboard.h"
 *     static muster_dashboard dash;
 *     // after WiFi is up (softAP or station):
 *     muster_dashboard_begin(&dash, &M, 80);
 *     // in loop(), after muster_poll():
 *     muster_dashboard_poll(&dash);
 *
 * The page polls /api/frame twice a second and decodes the binary snapshot in
 * the browser, so the board never formats JSON.
 *
 * Arduino-ESP32 only (it needs WebServer.h). Anywhere else, including AVR and
 * host tests, this header compiles to nothing. Compiles for ESP32 (see
 * firmware/examples/dashboard); NOT yet run on hardware.
 */
#ifndef MUSTER_DASHBOARD_H
#define MUSTER_DASHBOARD_H
#include "muster.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(__cplusplus)
#if !MUSTER_TELEMETRY
#error "muster_dashboard.h needs #define MUSTER_TELEMETRY 1 before muster.h"
#endif
#include <WebServer.h>
#include "muster_dashboard_page.h"

typedef struct
{
    muster_t *m;
    WebServer *server;
    uint8_t frame[MUSTER_TEL_FRAME_MAX];
    uint16_t len;
    uint32_t made_at;
} muster_dashboard;

static void muster_dashboard_begin(muster_dashboard *d, muster_t *m, uint16_t port)
{
    static WebServer *srv; /* one dashboard per board */
    d->m = m;
    d->len = 0;
    d->made_at = 0;
    srv = new WebServer(port); /* once, at start-up: the core itself never allocates */
    d->server = srv;
    srv->on("/", [srv]() {
        srv->sendHeader("Cache-Control", "no-store");
        srv->send_P(200, "text/html; charset=utf-8", MUSTER_DASHBOARD_PAGE);
    });
    srv->on("/api/frame", [d]() {
        /* a fresh snapshot at most every 250 ms, however many browsers poll */
        if (!d->len || millis() - d->made_at >= 250)
        {
            d->len = muster_telemetry_frame(d->m, d->frame, sizeof d->frame);
            d->made_at = millis();
        }
        d->server->sendHeader("Cache-Control", "no-store");
        d->server->sendHeader("X-Muster-Source", "lead (on-board)");
        if (!d->len)
        {
            d->server->send(204, "application/octet-stream", "");
            return;
        }
        d->server->setContentLength(d->len);
        d->server->send(200, "application/octet-stream", "");
        d->server->sendContent((const char *)d->frame, d->len);
    });
    srv->begin();
}

static void muster_dashboard_poll(muster_dashboard *d)
{
    if (d->server)
        d->server->handleClient();
}
#endif /* Arduino-ESP32 */
#endif

// SPDX-License-Identifier: GPL-3.0-only
// trio_lead.h - the lead role (the ESP32-C6). It:
//   * runs a WiFi access point and the dashboard (http://192.168.4.1/),
//   * keeps up to TRIO_SLOTS calls to the task in flight, so the placement
//     rule has something to balance across the workers,
//   * builds a per-board table from the results (each result carries the
//     board's own vital signs) and from its node table (alive / lost).
// The lead also holds the task, so if every worker is gone it computes alone
// (placement penalises it, so it only wins when nobody else can).
// Ran on an ESP32-C6 2026-11-07: dashboard, placement and failover worked.
#pragma once
#include <WebServer.h>
#include "dashboard.h"

#define TRIO_SLOTS 3            // calls in flight
#define TRIO_BOARDS 4           // dashboard rows: the lead + up to 3 workers
#define TRIO_LOG 10

static muster_t     M;
static muster_link  LINK;
static mespnow_ctx  RADIO;
static WebServer    server(80);

struct Board {
    bool     used;
    uint32_t uid;
    uint8_t  node;
    trio_result last;           // most recent result from this board
    uint32_t calls;             // results the lead has received from it
    uint32_t last_ms;           // when the last one arrived
    float    avg_busy;          // exponential average of busy_us
    bool     lead;
};
static Board    B[TRIO_BOARDS];
static int8_t   slot[TRIO_SLOTS] = {-1, -1, -1};
static uint16_t step = 0;
static uint32_t calls_ok = 0, calls_failed = 0, started_at;
static uint32_t rate_t0 = 0, rate_n0 = 0;
static float    rate_cps = 0;

struct LogLine { uint32_t t; char s[56]; };
static LogLine  logbuf[TRIO_LOG];
static uint8_t  log_n = 0;

static void say(const char *fmt, ...) {
    LogLine &l = logbuf[log_n % TRIO_LOG];
    va_list ap;
    l.t = millis();
    va_start(ap, fmt);
    vsnprintf(l.s, sizeof l.s, fmt, ap);
    va_end(ap);
    log_n++;
    Serial.printf("[%7lu] %s\n", (unsigned long)l.t, l.s);
}

static uint8_t lead_id(void) { return M.id; }

static Board *board_for(uint32_t uid) {
    for (auto &b : B) if (b.used && b.uid == uid) return &b;
    for (auto &b : B) if (!b.used) { b.used = true; b.uid = uid; return &b; }
    return nullptr;
}

static void on_event(uint8_t ev, uint8_t a, uint8_t b, void *) {
    const muster_node *n = (ev == MUSTER_EV_JOINED || ev == MUSTER_EV_LOST) ? muster_node_by_id(&M, a) : nullptr;
    switch (ev) {
    case MUSTER_EV_JOINED:
        if (n) { Board *bd = board_for(n->uid); if (bd) bd->node = a; }
        say("worker joined: node %u", a);
        break;
    case MUSTER_EV_LOST:   say("worker LOST: node %u (calls move to the others)", a); break;
    case MUSTER_EV_RETRY:  say("call retried: node %u did not answer", b); break;
    default: break;
    }
}

static void take_result(const uint8_t *buf, uint16_t n) {
    trio_result r;
    if (trio_get_result(buf, n, &r) != 0) return;
    Board *bd = board_for(r.uid);
    if (!bd) return;
    bd->node = r.node;
    bd->last = r;
    bd->calls++;
    bd->last_ms = millis();
    bd->avg_busy = bd->calls == 1 ? (float)r.busy_us : 0.9f * bd->avg_busy + 0.1f * (float)r.busy_us;
    trio_pwm(r.duty);                         // the lead's own pin mirrors the latest answer
}

// ------------------------------------------------------------- the web side
static const char *board_name(const Board &b, int idx, char *out, size_t cap) {
    if (b.lead) snprintf(out, cap, "Lead");
    else        snprintf(out, cap, "Worker %c", 'A' + (idx - 1));
    return out;
}

// snprintf returns the length it wanted, not what it wrote: clamp so a full
// buffer truncates the JSON instead of `sizeof js - n` wrapping round.
#define JS_APPEND(...) do { int k_ = snprintf(js + n, sizeof js - n, __VA_ARGS__); \
    if (k_ > 0) n = (n + (size_t)k_ < sizeof js) ? n + (size_t)k_ : sizeof js - 1; } while (0)

static void serve_state(void) {
    static char js[4096];
    size_t n = 0;
    uint32_t now = millis();
    uint8_t alive = muster_alive_count(&M);
    JS_APPEND(
        "{\"t\":%lu,\"ch\":%d,\"ssid\":\"%s\",\"alive\":%u,\"ok\":%lu,\"failed\":%lu,\"cps\":%.1f,\"step\":%u,"
        "\"radio\":{\"sent\":%lu,\"txfail\":%lu,\"rxdrop\":%lu},\"boards\":[",
        (unsigned long)(now - started_at), TRIO_CHANNEL, TRIO_AP_SSID, alive,
        (unsigned long)calls_ok, (unsigned long)calls_failed, (double)rate_cps, step,
        (unsigned long)RADIO.sent, (unsigned long)RADIO.send_failed, (unsigned long)RADIO.dropped);
    int rows = 0;
    for (int i = 0; i < TRIO_BOARDS; i++) {
        const Board &b = B[i];
        if (!b.used) continue;
        const muster_node *nd = muster_node_by_id(&M, b.node);
        char nm[16];
        board_name(b, i, nm, sizeof nm);
        // the lead describes itself; workers are described by their last result
        uint32_t heap = b.lead ? ESP.getFreeHeap() : b.last.free_heap;
        uint32_t up   = b.lead ? now : b.last.uptime_ms;
        uint32_t tasks = b.lead ? trio_tasks_done : b.last.tasks;
        JS_APPEND(
            "%s{\"slot\":%d,\"name\":\"%s\",\"lead\":%d,\"node\":%u,\"uid\":\"%08lX\",\"chip\":%u,\"alive\":%d,"
            "\"cpu\":%u,\"heap\":%lu,\"up\":%lu,\"tasks\":%lu,\"calls\":%lu,\"duty\":%u,\"val\":%.3f,"
            "\"busy\":%lu,\"avg\":%.0f,\"age\":%ld,\"inflight\":%u,\"queued\":%lu}",
            rows++ ? "," : "", i, nm, b.lead, b.node, (unsigned long)b.uid, b.lead ? TRIO_CHIP : b.last.chip,
            b.lead ? 1 : (nd && nd->alive ? 1 : 0),
            b.lead ? (unsigned)getCpuFrequencyMhz() : b.last.cpu_mhz,
            (unsigned long)heap, (unsigned long)up, (unsigned long)tasks, (unsigned long)b.calls,
            b.lead ? trio_last_duty : b.last.duty, (double)b.last.value,
            (unsigned long)b.last.busy_us, (double)b.avg_busy,
            b.calls ? (long)(now - b.last_ms) : -1L,
            nd ? nd->inflight : 0, nd ? (unsigned long)nd->queued : 0UL);
    }
    JS_APPEND("],\"log\":[");
    uint8_t shown = log_n < TRIO_LOG ? log_n : TRIO_LOG;
    for (uint8_t k = 0; k < shown; k++) {                      // oldest first
        const LogLine &l = logbuf[(log_n - shown + k) % TRIO_LOG];
        JS_APPEND("%s{\"t\":%lu,\"m\":\"%s\"}", k ? "," : "",
                      (unsigned long)(l.t - started_at), l.s);
    }
    JS_APPEND("]}");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", js);
}

static void serve_page(void) { server.send_P(200, "text/html", TRIO_DASH_HTML); }

// ----------------------------------------------------------------- the rest
void trio_setup(void) {
    Serial.begin(115200);
    delay(300);
    started_at = millis();
    trio_board_begin(lead_id);

    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA);                               // AP for the dashboard; the STA side carries ESP-NOW
    WiFi.softAP(TRIO_AP_SSID, TRIO_AP_PASS, TRIO_CHANNEL);
    if (muster_espnow_begin(&LINK, &RADIO, 0, TRIO_CHANNEL, /*ap_running=*/1) != 0)
        Serial.println("ESP-NOW init FAILED");
    muster_begin(&M, 0, /*is_lead=*/1, ESP.getChipCores(), ESP.getHeapSize() / 1024,
                 getCpuFrequencyMhz(), MUSTER_CAP_WIFI, &LINK, trio_ms);   // C6: no FPU flag
    muster_register_task(&M, TRIO_T_WAVE, trio_wave_tr, TRIO_COST);
    muster_on_event(&M, on_event, nullptr);

    Board *me = board_for(trio_uid());
    me->lead = true;
    me->node = 0;

    server.on("/", serve_page);
    server.on("/api/state", serve_state);
    server.begin();
    Serial.printf("\nmuster trio LEAD (%s)  pwm pin=%d  channel=%d\n", TRIO_CHIP_NAME, TRIO_PWM_PIN, TRIO_CHANNEL);
    Serial.printf("join WiFi \"%s\" (password \"%s\"), then open http://%s/\n",
                  TRIO_AP_SSID, TRIO_AP_PASS, WiFi.softAPIP().toString().c_str());
}

static void issue_and_collect(void) {
    uint8_t buf[MUSTER_MAX_RET];
    uint32_t now = millis();
    for (auto &h : slot) {
        uint16_t n = 0;
        int8_t st;
        if (h < 0) continue;
        st = muster_result(&M, h, buf, sizeof buf, &n);
        if (st == MUSTER_PENDING) continue;
        h = -1;
        if (st == MUSTER_OK) { calls_ok++; take_result(buf, n); }
        else { calls_failed++; if (calls_failed < 20 || calls_failed % 50 == 0) say("call failed (%d)", st); }
    }
    if (now - rate_t0 >= 2000) {
        rate_cps = (calls_ok - rate_n0) * 1000.0f / (float)(now - rate_t0);
        rate_t0 = now;
        rate_n0 = calls_ok;
    }
    static uint32_t last_issue = 0;
    if (now - last_issue < TRIO_CALL_MS) return;
    for (auto &h : slot) {
        if (h >= 0) continue;
        uint8_t a[4];
        muster_wbuf w;
        muster_w_init(&w, a, sizeof a);
        trio_encode_args(&w, step, TRIO_TERMS);
        h = muster_call_async(&M, TRIO_T_WAVE, a, w.n);
        if (h >= 0) { step++; last_issue = now; }
        break;                                             // at most one new call per pass
    }
}

void trio_loop(void) {
    static uint32_t last_print = 0;
    muster_poll(&M);
    issue_and_collect();
    server.handleClient();                                 // short; keeps the loop responsive
    if (millis() - last_print >= 3000) {
        last_print = millis();
        Serial.printf("[%7lu] boards alive=%u  ok=%lu failed=%lu  %.1f calls/s  radio tx=%lu txfail=%lu rxdrop=%lu\n",
                      (unsigned long)millis(), muster_alive_count(&M), (unsigned long)calls_ok,
                      (unsigned long)calls_failed, (double)rate_cps, (unsigned long)RADIO.sent,
                      (unsigned long)RADIO.send_failed, (unsigned long)RADIO.dropped);
    }
}

/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 The Muster contributors */
/* test_sim_cluster.c - the cluster-in-a-chip used for the Wokwi simulation
 * (examples/sim_cluster) run on the host: the same code, driven through the
 * scenarios the buttons in the simulator trigger.                           */
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "rig.h"
#include "../examples/sim_cluster/src/cluster.h"

static sim_cluster C;
static char logbuf[16384];
static void capture(const char *line)
{
    size_t n = strlen(logbuf);
    if (n + strlen(line) + 2 < sizeof logbuf) { strcpy(logbuf + n, line); strcat(logbuf, "\n"); }
}
static int saw(const char *s) { return strstr(logbuf, s) != 0; }
static int count_running(void) { int i, n = 0; for (i = 0; i < SC_NODES; i++) n += C.running[i]; return n; }
static int runner(void) { int i; for (i = 0; i < SC_NODES; i++) if (C.running[i]) return i; return -1; }
static void run(uint32_t ms) { while (ms--) { g_now++; sc_step(&C); } }

int main(void)
{
    int first, second, blinking, i;
    uint32_t done_before;
    char s[160];

    printf("\nsim_cluster (Wokwi demo logic) on the host\n\n");
    sc_init(&C, now_ms, capture);

    printf("1. start up\n");
    run(3500);
    CHECK(muster_alive_count(&C.node[0]) == 3, "lead sees both workers");
    CHECK(C.node[1].id != C.node[2].id && C.node[1].id >= 1 && C.node[2].id >= 1,
          "workers took their IDs from the (fake) chip ID");
    CHECK(count_running() == 1 && runner() > 0, "the module runs on exactly one board, a worker");
    CHECK(C.done >= 1 && C.failed == 0, "square() calls complete");
    CHECK(saw("ran on node1") || saw("ran on node2"), "and they ran on a worker");
    CHECK(!saw("ran on node0"), "never on the lead (it does not hold the task)");
    first = runner();
    blinking = 0;
    for (i = 0; i < 16; i++) blinking += sc_led(&C, (uint8_t)first, (uint32_t)(i * 125));
    CHECK(blinking > 0 && blinking < 16, "that board's LED blinks");
    CHECK(sc_led(&C, (uint8_t)(3 - first), 0) == 1, "the other worker's LED is steady on");
    sc_status(&C, s, sizeof s);
    CHECK(strstr(s, "BLINK") != 0, "status line says who is running it");

    printf("2. pull the cable of the board running the module\n");
    sc_set_plugged(&C, (uint8_t)first, 0);
    run(MUSTER_NODE_TIMEOUT_MS + 800);
    second = runner();
    CHECK(saw("LOST"), "the lead notices");
    CHECK(count_running() == 1 && second == 3 - first, "the module moved to the other worker");
    CHECK(sc_led(&C, (uint8_t)first, 5) == 0, "the pulled board's LED is off");
    done_before = C.done;
    run(2500);
    CHECK(C.done > done_before, "square() still works, on the survivor");

    printf("3. pull the second cable too\n");
    sc_set_plugged(&C, (uint8_t)second, 0);
    run(MUSTER_NODE_TIMEOUT_MS + 1500);
    CHECK(count_running() == 1 && runner() == 0, "with no workers left the lead runs the module itself");
    CHECK(C.failed >= 1 && saw("FAILED"), "square() fails cleanly: nobody holds it");

    printf("4. plug both back\n");
    sc_set_plugged(&C, (uint8_t)first, 1);
    sc_set_plugged(&C, (uint8_t)second, 1);
    done_before = C.done;
    run(4000);
    CHECK(muster_alive_count(&C.node[0]) == 3, "both workers rejoin");
    CHECK(count_running() == 1 && runner() == 0,
          "the module stays where it is (moving it would lose its state)");
    CHECK(C.done > done_before, "square() works again");

    printf("\n--- what the serial monitor would show ---\n%s", logbuf);
    return rig_done();
}

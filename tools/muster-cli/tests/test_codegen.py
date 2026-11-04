# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
import re

import pytest

from conftest import BOARDS, FIRMWARE_SRC, SOURCE, host_cxx
from muster_cli import pipeline as P
from muster_cli.codegen import generate_header


@pytest.fixture(scope="module")
def project(tmp_path_factory):
    out = tmp_path_factory.mktemp("quad")
    r = P.make(P.baseline(BOARDS, SOURCE), BOARDS, SOURCE, out, wiring=False)
    assert r["ok"], r["errors"]
    return out, r


def test_project_files(project):
    out, r = project
    files = set(r["project"]["files"])
    for f in ("platformio.ini", "plan.json", "src/main.cpp", "src/muster_runtime.cpp",
              "include/muster.h", "include/muster_gen.h", "include/muster_plan.h",
              "include/muster_annotations.h", "include/muster_link_twai.h",
              "include/muster_link_mcp2515.h"):
        assert f in files
    assert r["project"]["envs"] == ["esp32_a", "esp32_b", "nano"]
    assert (out / "include" / "muster.h").read_bytes() == (FIRMWARE_SRC / "muster.h").read_bytes()


def test_ini_per_board(project):
    out, _ = project
    ini = (out / "platformio.ini").read_text()
    nano = ini[ini.index("[env:nano]"):]
    assert "-DMUSTER_LEAD=1" in ini[ini.index("[env:esp32_a]"):ini.index("[env:esp32_b]")]
    assert "-DMUSTER_GEN_LINK_MCP2515" in nano and "-DMUSTER_GEN_MCP_CS=10" in nano
    assert "autowp/autowp-mcp2515" in nano and "-DMUSTER_RESIDENT_MASK=0x4ul" in nano
    assert "-DMUSTER_GEN_CAN_TX=5" in ini and "-DMUSTER_GEN_CAN_RX=4" in ini   # boards.yaml
    assert "-include" not in ini


def test_sketch_gets_includes_after_last_task(project):
    out, r = project
    lines = (out / "src" / "main.cpp").read_text().splitlines()
    assert "muster_annotations.h" in lines[0]
    gen = next(i for i, l in enumerate(lines) if "muster_gen.h" in l and l.startswith("#include"))
    src = SOURCE.read_text().splitlines()
    last_task_end = max(t["end_line"] for t in P.analyse(SOURCE)["tasks"])
    assert lines[1:gen - 1] == src[:last_task_end]          # +1 for the annotation line


def test_header_registers_only_resident_and_costs_all(project):
    out, _ = project
    h = (out / "include" / "muster_gen.h").read_text()
    assert len(re.findall(r"muster_set_cost\(&muster_rt", h)) == 3
    assert "#if (MUSTER_RESIDENT_MASK & (1ul << MUSTER_TASK_ID_IMU_FILTER))" in h
    for fn in ("muster_imu_filter(", "muster_imu_filter_async(", "muster_imu_filter_result(",
               "muster_tr_gait_planner(", "mg_put_Gait(", "mg_get_Vec3("):
        assert fn in h


def test_regenerate_is_stable(project, tmp_path):
    out, _ = project
    first = (out / "include" / "muster_gen.h").read_text()
    P.make(P.baseline(BOARDS, SOURCE), BOARDS, SOURCE, tmp_path, wiring=False)
    assert (tmp_path / "include" / "muster_gen.h").read_text() == first


def test_rejected_plan_writes_no_project(tmp_path):
    plan = P.baseline(BOARDS, SOURCE)
    plan["lead"] = "ghost"
    r = P.make(plan, BOARDS, SOURCE, tmp_path, wiring=False)
    assert not r["ok"] and "project" not in r
    assert sorted(p.name for p in tmp_path.iterdir()) == ["plan.json"]


def test_void_and_no_arg_stubs(analyse_text):
    h = generate_header(analyse_text("MUSTER_TASK(cost = 1)\nvoid ping() {}\n"
                                     "MUSTER_TASK(cost = 1)\nbool led(bool on) { return on; }\n"))
    assert "static inline int8_t muster_ping(uint32_t mg_timeout_ms = 1000)" in h
    assert "static inline int8_t muster_ping_result(int8_t mg_h)" in h
    assert "muster_put_u8(&mg_w, (uint8_t)(on ? 1 : 0));" in h
    assert "muster_get_u8(&mg_r) != 0;" in h


HARNESS = r"""
#define MUSTER_IMPLEMENTATION
#include "main.cpp"
#include "muster_link_loopback.h"
#include <stdio.h>

static lb_bus bus; static lb_ctx ctx; static muster_link lk;
muster_link *muster_gen_external_link(void) { lb_attach(&bus, &ctx, &lk, 0, 8, 500); return &lk; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s\n", #c); fails++; } } while (0)

int main() {
    setup();
    Vec3 acc = {0.1f, 0.2f, 9.8f}, gyr = {1.0f, 2.0f, 3.0f}, out = {0, 0, 0};
    CHECK(muster_imu_filter(acc, gyr, &out) == MUSTER_OK);
    CHECK(fabsf(out.x - 0.982f) < 1e-4f && fabsf(out.z - 3.136f) < 1e-4f);
    Gait g; memset(&g, 0, sizeof g);
    CHECK(muster_gait_planner(1.0f, 0.5f, &g) == MUSTER_OK);
    Gait want = gait_planner(1.0f, 0.5f);
    CHECK(memcmp(&g, &want, sizeof g) == 0);
    int16_t v = 0;
    int8_t h = muster_sensor_read_async(7);
    CHECK(h >= 0);
    int8_t st = MUSTER_PENDING;
    for (int i = 0; i < 100 && st == MUSTER_PENDING; i++) { st = muster_sensor_read_result(h, &v); muster_loop(); }
    CHECK(st == MUSTER_OK && v == 107);
    uint8_t junk[3] = {1, 2, 3}, ret[64];
    CHECK(muster_tr_imu_filter(junk, sizeof junk, ret, sizeof ret) == -1);  /* wrong size */
    printf("%s\n", fails ? "HARNESS FAILED" : "HARNESS OK");
    return fails;
}
"""

ARDUINO_STUB = r"""
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>
#define F(s) (s)
static uint32_t g_ms;
static inline uint32_t millis(void) { return g_ms++; }      /* time moves on every read */
static inline int analogRead(uint8_t ch) { return 100 + ch; }
struct StubSerial {
    void begin(unsigned long) {}
    template <class T> void print(T) {} template <class T> void print(T, int) {}
    template <class T> void println(T) {} template <class T> void println(T, int) {}
    void println() {}
};
static StubSerial Serial;
"""


def test_generated_glue_round_trip_on_host(project, tmp_path):
    """Compile the generated sketch + muster_gen.h for the host, single lead
    board on a loopback bus, and call every task through its stub."""
    cxx = host_cxx()
    if cxx is None:
        pytest.skip("no host C++ compiler (set MUSTER_HOST_CXX, or MUSTER_HOST_CXX=wsl)")
    out, _ = project
    (tmp_path / "Arduino.h").write_text(ARDUINO_STUB)
    (tmp_path / "harness.cpp").write_text(HARNESS)
    args = ["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-I", str(tmp_path), "-I", str(out / "src"), "-I", str(out / "include"),
            "-I", str(FIRMWARE_SRC),
            "-DMUSTER_NODE_ID=0", "-DMUSTER_LEAD=1", "-DMUSTER_RESIDENT_MASK=0x7ul",
            "-DMUSTER_GEN_CORES=2", "-DMUSTER_GEN_RAM_KB=512", "-DMUSTER_GEN_CLOCK_MHZ=240",
            "-DMUSTER_GEN_FLAGS=1", "-DMUSTER_GEN_LINK_EXTERNAL",
            str(tmp_path / "harness.cpp"), "-o", "harness"]
    r = cxx(tmp_path, args)
    assert r.returncode == 0, r.stdout + r.stderr
    run = cxx(tmp_path, ["./harness"])
    assert "HARNESS OK" in run.stdout, run.stdout + run.stderr

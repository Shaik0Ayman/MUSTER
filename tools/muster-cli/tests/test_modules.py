# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""Modules (long-running units the lead assigns) and shared images (one build
for every board of a type, node ID from the chip)."""
import copy

import pytest

from conftest import FIRMWARE_SRC, ROOT, host_cxx
from muster_cli import pipeline as P
from muster_cli.analyze import analyze_file
from muster_cli.validate import validate

EX = ROOT / "examples" / "quadruped_modules"
BOARDS, SOURCE = EX / "boards.yaml", EX / "main.cpp"

TASK = "MUSTER_TASK(cost = 10, ram = 1)\nint16_t twice(int16_t x) { return (int16_t)(2 * x); }\n"


def errors_of(plan, analysis, inventory):
    return validate(plan, analysis, inventory)[0]


@pytest.fixture(scope="module")
def example(tmp_path_factory):
    out = tmp_path_factory.mktemp("quadmod")
    plan = P.baseline(BOARDS, SOURCE)
    r = P.make(plan, BOARDS, SOURCE, out, wiring=False)
    assert r["ok"], r["errors"]
    return out, r, plan


# ---------------------------------------------------------------- analysis
def test_modules_found_in_sketch_and_local_header(example):
    a = P.analyse(SOURCE)
    assert [t["name"] for t in a["tasks"]] == ["gait_planner", "sensor_read"]
    assert [(m["name"], m["count"], m["needs"], m["ram_kb"]) for m in a["modules"]] == [
        ("status_led", 1, [], 1), ("gait_controller", 1, ["fpu"], 16)]
    assert a["local_headers"] == ["modules/status_led.h"]
    assert a["diagnostics"] == []
    # glue goes after the last declaration, before the definition that uses the stubs
    lines = SOURCE.read_text().splitlines()
    assert "gait_controller(uint8_t event);" in lines[a["insert_line"] - 1]


def test_module_signature_and_keys_are_checked(tmp_path):
    src = tmp_path / "m.cpp"
    src.write_text("#include <Arduino.h>\n" + TASK +
                   "MUSTER_MODULE(cost = 3)\nint bad(int x) { return x; }\n"
                   "MUSTER_MODULE(count = 9)\nint8_t big(uint8_t e) { return e; }\n")
    mods = {m["name"]: m for m in analyze_file(src)["modules"]}
    assert any("int8_t bad(uint8_t event)" in e for e in mods["bad"]["errors"])
    assert any("unknown annotation key 'cost'" in e for e in mods["bad"]["errors"])
    assert any("count must be 1..8" in e for e in mods["big"]["errors"])


def test_module_in_header_is_not_counted_twice(tmp_path):
    (tmp_path / "m.h").write_text("MUSTER_MODULE(ram = 1)\nint8_t one(uint8_t e);\n")
    src = tmp_path / "m.cpp"
    src.write_text('#include <Arduino.h>\n#include "m.h"\n' + TASK +
                   "int8_t one(uint8_t e) { return (int8_t)e; }\n")
    a = analyze_file(src)
    assert [m["name"] for m in a["modules"]] == ["one"] and a["local_headers"] == ["m.h"]


# ---------------------------------------------------------------- planning
def test_baseline_offers_modules_to_esp32s_only(example):
    _, _, plan = example
    assert plan["modules"] == [
        {"module": "status_led", "boards": ["esp32_a", "esp32_b", "esp32_c"], "count": 1},
        {"module": "gait_controller", "boards": ["esp32_a", "esp32_b", "esp32_c"], "count": 1}]


def _check(plan):
    return P.check(plan, BOARDS, SOURCE)


def test_validator_rejects_bad_module_plans(example):
    _, _, plan = example

    def mutated(fn):
        p = copy.deepcopy(plan)
        fn(p)
        return _check(p)["errors"]

    e = mutated(lambda p: p["modules"][0]["boards"].append("nano"))
    assert any("'nano', but Arduino Nano" in x and "cannot run modules" in x for x in e)
    e = mutated(lambda p: p["modules"][1]["boards"].__setitem__(slice(None), ["esp32_a"]) or
                p["modules"][1].update(count=2))
    assert any("must run on 2 board(s) at once but only 1 can hold it" in x for x in e)
    e = mutated(lambda p: p["modules"].pop())
    assert any("module 'gait_controller' is missing from the plan" in x for x in e)
    e = mutated(lambda p: p.pop("modules"))
    assert sum("is missing from the plan" in x for x in e) == 2
    e = mutated(lambda p: p["modules"].append({"module": "ghost", "boards": ["esp32_a"]}))
    assert any("unknown module 'ghost'" in x for x in e)
    e = mutated(lambda p: p["modules"][0].update(count=0))
    assert any("count must be a whole number >= 1" in x for x in e)


def test_shared_image_ram_is_the_union(example):
    """gait_planner (24 KB) + gait_controller (16 KB) on one ESP32 image is fine;
    the budget is checked against the whole image, not per board name."""
    _, _, plan = example
    assert _check(plan)["errors"] == []
    a, inv = P.analyse(SOURCE), P.load_inventory(BOARDS)
    a["modules"][1]["ram_kb"] = 600           # more than an ESP32-S3 has
    assert any("shared image" not in x and "gait_controller" in x and "600 KB" in x
               for x in errors_of(plan, a, inv))


def test_sharing_widening_warning(example):
    _, _, plan = example
    p = copy.deepcopy(plan)
    for a in p["assignments"]:
        if a["task"] == "gait_planner":
            a["boards"] = ["esp32_a", "esp32_b"]       # not esp32_c, but they share an image
    w = _check(p)["warnings"]
    assert any("share one image" in x and "'esp32_b', 'esp32_c'" in x for x in w)


# ---------------------------------------------------------------- generation
def test_shared_project_envs_and_ids(example):
    out, r, _ = example
    assert r["project"]["envs"] == ["esp32_a", "esp32_s3_devkitc_1", "nano"]
    assert r["project"]["images"]["esp32_s3_devkitc_1"] == ["esp32_b", "esp32_c"]
    ini = (out / "platformio.ini").read_text()
    lead = ini[ini.index("[env:esp32_a]"):ini.index("; esp32-s3")]
    shared = ini[ini.index("[env:esp32_s3_devkitc_1]"):ini.index("; nano")]
    nano = ini[ini.index("[env:nano]"):]
    assert "-DMUSTER_NODE_ID=0" in lead and "-DMUSTER_LEAD=1" in lead
    assert "-DMUSTER_MODULE_COUNT_GAIT_CONTROLLER=1" in lead and "-DMUSTER_MODULE_MASK=0x3" in lead
    assert "-DMUSTER_ID_FROM_UID=1" in shared and "-DMUSTER_LEAD=0" in shared
    assert "COUNT" not in shared and "upload_port" not in shared     # only the lead counts boards
    assert "-DMUSTER_NODE_ID=200" not in shared and "-DMUSTER_ID_FROM_UID" not in nano
    assert "-DMUSTER_NODE_ID=200" in nano and "-DMUSTER_MODULE_MASK=0x0" in nano
    assert (out / "src" / "modules" / "status_led.h").is_file()
    assert "MUSTER_MOD_RUN" in (out / "include" / "muster_annotations.h").read_text()


def test_header_glue_for_modules(example):
    out, _, _ = example
    h = (out / "include" / "muster_gen.h").read_text()
    assert "muster_set_uid(&muster_rt" in h and "esp_efuse_mac_get_default" in h
    assert "static inline void muster_modules_step_(void)" in h
    assert "muster_register_module(&muster_rt, 1);" in h
    assert "muster_set_module(&muster_rt, 0, MUSTER_MODULE_COUNT_STATUS_LED);" in h
    assert "gait_controller(MUSTER_MOD_START) < 0" in h          # declining is supported
    plan_h = (out / "include" / "muster_plan.h").read_text()
    assert "MUSTER_MODULE_ID_GAIT_CONTROLLER 1u" in plan_h


def test_regenerate_is_stable(example, tmp_path):
    out, _, plan = example
    P.make(plan, BOARDS, SOURCE, tmp_path, wiring=False)
    for f in ("platformio.ini", "include/muster_gen.h", "src/main.cpp"):
        assert (tmp_path / f).read_text() == (out / f).read_text()


def test_upload_one_command_per_board_with_ports_lead_last(example):
    out, _, _ = example
    steps = P.upload(BOARDS, out, dry_run=True)
    cmds = {s["board"]: s["command"] for s in steps}
    assert steps[-1]["board"] == "esp32_a"
    assert "-e esp32_s3_devkitc_1 -t upload --upload-port /dev/ttyUSB1" in cmds["esp32_b"]
    assert "-e esp32_s3_devkitc_1 -t upload --upload-port /dev/ttyUSB2" in cmds["esp32_c"]
    assert cmds["esp32_a"].endswith("-e esp32_a -t upload")
    assert cmds["nano"].endswith("-e nano -t upload")


# ---------------------------------------------------------------- host round trip
MODULE_SKETCH = """#include <Arduino.h>
""" + TASK + """MUSTER_MODULE(ram = 1, count = 1)
int8_t worker_mod(uint8_t event);
void setup() { Serial.begin(115200); }
void loop() {}
int g_starts, g_runs, g_stops, g_last;
int8_t worker_mod(uint8_t event) {
    if (event == MUSTER_MOD_START) g_starts++;
    if (event == MUSTER_MOD_RUN) {
        g_runs++;
        int16_t r = 0;
        if (muster_twice(21, &r, 50) == MUSTER_OK) g_last = r;   // modules may call tasks
    }
    if (event == MUSTER_MOD_STOP) g_stops++;
    return 0;
}
"""

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
    CHECK(muster_setup() == 0);
    for (int i = 0; i < 20; i++) muster_loop();
    CHECK(g_starts == 1 && g_runs >= 10 && g_stops == 0);     /* the lead gave it to itself */
    CHECK(g_last == 42);                                      /* and it called a task */
    muster_set_module(&muster_rt, MUSTER_MODULE_ID_WORKER_MOD, 0);   /* take it away */
    for (int i = 0; i < 20; i++) muster_loop();
    int runs = g_runs;
    for (int i = 0; i < 20; i++) muster_loop();
    CHECK(g_stops == 1 && g_runs == runs);
    muster_set_module(&muster_rt, MUSTER_MODULE_ID_WORKER_MOD, 1);   /* and give it back */
    for (int i = 0; i < 20; i++) muster_loop();
    CHECK(g_starts == 2 && g_runs > runs);
    printf("%s\n", fails ? "HARNESS FAILED" : "HARNESS OK");
    return fails;
}
"""


def test_generated_module_glue_runs_on_host(tmp_path):
    from test_codegen import ARDUINO_STUB
    cxx = host_cxx()
    if cxx is None:
        pytest.skip("no host C++ compiler (set MUSTER_HOST_CXX, or MUSTER_HOST_CXX=wsl)")
    (tmp_path / "boards.yaml").write_text(
        "boards:\n  - id: lead\n    type: esp32-s3-devkitc-1\n")
    sk = tmp_path / "sk"
    sk.mkdir()
    (sk / "main.cpp").write_text(MODULE_SKETCH)
    out = tmp_path / "out"
    r = P.make(P.baseline(tmp_path / "boards.yaml", sk / "main.cpp"), tmp_path / "boards.yaml",
               sk / "main.cpp", out, wiring=False)
    assert r["ok"], r["errors"]
    (tmp_path / "Arduino.h").write_text(ARDUINO_STUB)
    (tmp_path / "harness.cpp").write_text(HARNESS)
    args = ["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-I", str(tmp_path), "-I", str(out / "src"), "-I", str(out / "include"),
            "-I", str(FIRMWARE_SRC),
            "-DMUSTER_NODE_ID=0", "-DMUSTER_LEAD=1", "-DMUSTER_RESIDENT_MASK=0x1ul",
            "-DMUSTER_GEN_CORES=2", "-DMUSTER_GEN_RAM_KB=512", "-DMUSTER_GEN_CLOCK_MHZ=240",
            "-DMUSTER_GEN_FLAGS=1", "-DMUSTER_GEN_LINK_EXTERNAL",
            "-DMUSTER_MODULE_MASK=0x1", "-DMUSTER_MODULE_COUNT_WORKER_MOD=1",
            str(tmp_path / "harness.cpp"), "-o", "harness"]
    c = cxx(tmp_path, args)
    assert c.returncode == 0, c.stdout + c.stderr
    run = cxx(tmp_path, ["./harness"])
    assert "HARNESS OK" in run.stdout, run.stdout + run.stderr

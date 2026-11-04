# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
import pytest

from conftest import SOURCE
from muster_cli.analyze import analyze_file


def test_quadruped_tasks_and_sizes():
    a = analyze_file(SOURCE)
    t = {x["name"]: x for x in a["tasks"]}
    assert list(t) == ["gait_planner", "imu_filter", "sensor_read"]
    assert (t["gait_planner"]["args_bytes"], t["gait_planner"]["ret_bytes"]) == (8, 32)
    assert (t["imu_filter"]["args_bytes"], t["imu_filter"]["ret_bytes"]) == (24, 12)
    assert (t["sensor_read"]["args_bytes"], t["sensor_read"]["ret_bytes"]) == (1, 2)
    assert a["types"]["Vec3"]["size"] == 12 and a["types"]["Gait"]["size"] == 32
    assert all(not x["errors"] for x in a["tasks"])
    assert t["imu_filter"]["needs"] == ["fpu"] and t["imu_filter"]["cost"] == 300
    assert a["insert_line"] == t["sensor_read"]["end_line"]
    assert a["diagnostics"] == [] and not a["has_gen_include"]


def test_first_analyser_contract_keys_kept():
    keys = {"name", "return_type", "params", "pointer_params", "globals_touched",
            "needs", "cost", "ram_kb"}
    for t in analyze_file(SOURCE)["tasks"]:
        assert keys <= set(t)
    assert "globals" in analyze_file(SOURCE)


@pytest.mark.parametrize("sig, expect", [
    ("int16_t f(int x)", "is int; use int16_t or int32_t"),
    ("int16_t f(unsigned x)", "is unsigned int; use uint16_t or uint32_t"),
    ("int16_t f(long x)", "is long; use int32_t"),
    ("int16_t f(short x)", "is short; use int16_t"),
    ("int16_t f(char c)", "is char; use uint8_t or int8_t"),
    ("double f(float x)", "return value is double; use float"),
    ("int16_t f(size_t n)", "is size_t; use uint16_t or uint32_t"),
    ("int16_t f(float *p)", "is a pointer"),
    ("int16_t f(const float &v)", "is a reference"),
    ("int16_t f(float v[4])", "is an array (float[4]); C passes arrays as pointers"),
    ("int16_t f(E e)", "is an enum"),
    ("int16_t f(U u)", "is a union"),
    ("int16_t f(Bad b)", "field Bad.n is int"),
    ("int16_t f(Bits b)", "bit-field"),
    ("int16_t f(myint v)", "is int"),
])
def test_rejected_types(analyse_text, sig, expect):
    body = ("enum E { A, B };\nunion U { float f; uint32_t u; };\n"
            "struct Bad { int n; };\nstruct Bits { uint8_t a : 3; };\ntypedef int myint;\n"
            f"MUSTER_TASK(cost = 1)\n{sig} {{ return 0; }}\n")
    errors = analyse_text(body)["tasks"][0]["errors"]
    assert any(expect in e for e in errors), errors


def test_accepted_types(analyse_text):
    body = ("typedef uint8_t channel_t;\nstruct In { int16_t a[2]; bool b; };\n"
            "struct Out { In in; uint32_t c; int8_t d[3]; };\n"
            "MUSTER_TASK(cost = 1)\nOut f(const float x, channel_t ch, In in, bool on) "
            "{ Out o = {}; (void)x; (void)ch; (void)on; o.in = in; return o; }\n"
            "MUSTER_TASK(cost = 1)\nvoid g() {}\n")
    a = analyse_text(body)
    f, g = a["tasks"]
    assert f["errors"] == [] and g["errors"] == []
    assert f["args_bytes"] == 4 + 1 + 5 + 1        # float, uint8 alias, In(2*2+1), bool
    assert f["ret_bytes"] == 5 + 4 + 3
    assert g["args_bytes"] == 0 and g["ret_bytes"] == 0 and g["returns"] == {"kind": "void"}


def test_globals_and_static_locals(analyse_text):
    body = ("int counter;\nconst float GAIN = 2.0f;\nstatic const int16_t TABLE[2] = {1, 2};\n"
            "MUSTER_TASK(cost = 1)\nint16_t f(int16_t x) { counter++; static int16_t k = 0; "
            "k++; return (int16_t)(GAIN * x + TABLE[0] + k); }\n")
    a = analyse_text(body)
    t = a["tasks"][0]
    assert t["globals_touched"] == ["counter"]            # consts are fine
    assert a["globals"] == ["counter"]
    assert any("static local(s) k" in w for w in t["warnings"])


def test_annotation_errors(analyse_text):
    t = analyse_text("MUSTER_TASK(needs = fpu|laser, cost = 0, colour = red, ram = x)\n"
                     "void f() {}\n")["tasks"][0]
    joined = " ".join(t["errors"])
    for frag in ("unknown need(s) laser", "cost must be 1..65535",
                 "unknown annotation key 'colour'", "ram = x is not an integer"):
        assert frag in joined


def test_missing_headers_are_stubbed_and_errors_reported(analyse_text):
    a = analyse_text("#include <Servo.h>\n#include \"lib/thing.h\"\n"
                     "MUSTER_TASK(cost = 1)\nvoid f() { undefined_call(); muster_f(); }\n")
    assert set(a["stubbed_headers"]) == {"Servo.h", "lib/thing.h"}
    assert len(a["diagnostics"]) == 1 and "undefined_call" in a["diagnostics"][0]
    assert a["tasks"][0]["name"] == "f"


def test_existing_gen_include_detected(analyse_text):
    a = analyse_text("MUSTER_TASK(cost = 1)\nvoid f() {}\n#include \"muster_gen.h\"\n"
                     "void loop() { muster_f(); }\n")
    assert a["has_gen_include"] and a["diagnostics"] == []


def test_member_and_duplicate_tasks(analyse_text):
    a = analyse_text("struct S { MUSTER_TASK(cost = 1) void m() {} };\n"
                     "namespace n { MUSTER_TASK(cost = 1) void g() {} }\n"
                     "MUSTER_TASK(cost = 1) void g() {}\n")
    errs = " ".join(e for t in a["tasks"] for e in t["errors"])
    assert "defined twice" in errs

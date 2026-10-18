import copy

import pytest
import yaml

from conftest import BOARDS, SOURCE
from muster_cli import pipeline as P
from muster_cli.analyze import analyze_file
from muster_cli.partition import baseline_partition
from muster_cli.validate import validate


@pytest.fixture(scope="module")
def ctx():
    a = analyze_file(SOURCE)
    inv = P.load_inventory(BOARDS)
    return a, inv, baseline_partition(a, inv)


def errs(plan, a, inv):
    return " | ".join(validate(plan, a, inv)[0])


def test_baseline_is_valid(ctx):
    a, inv, plan = ctx
    errors, warnings = validate(plan, a, inv)
    assert errors == []
    assert plan["lead"] == "esp32_a"
    assert any("mixed logic levels" in w for w in warnings)


@pytest.mark.parametrize("mutate, expect", [
    (lambda p: p.update(lead="ghost"), "lead board 'ghost' is not in the inventory"),
    (lambda p: p["assignments"][0]["boards"].append("nano"), "needs fpu but 'nano'"),
    (lambda p: p["assignments"][0]["boards"].append("esp32_c"), "unknown board 'esp32_c'"),
    (lambda p: p["assignments"].pop(), "task 'sensor_read' is missing"),
    (lambda p: p["assignments"].append(copy.deepcopy(p["assignments"][0])), "appears twice"),
    (lambda p: p["assignments"][2]["boards"].clear(), "not resident on any board"),
    (lambda p: p["assignments"][2]["boards"].append("nano"), "lists a board twice"),
    (lambda p: p["assignments"].append({"task": "fly", "boards": ["nano"]}), "unknown task 'fly'"),
    (lambda p: p.update(assignments="all"), "must be a list"),
    (lambda p: p["assignments"].__setitem__(0, {"task": "x"}), "assignment #0 must be"),
])
def test_plan_errors(ctx, mutate, expect):
    a, inv, plan = ctx
    bad = copy.deepcopy(plan)
    mutate(bad)
    assert expect in errs(bad, a, inv)


def test_single_board_warning(ctx):
    a, inv, plan = ctx
    p = copy.deepcopy(plan)
    p["assignments"][0]["boards"] = ["esp32_a"]
    assert any("lives on one board only" in w for w in validate(p, a, inv)[1])


def test_not_a_plan(ctx):
    a, inv, _ = ctx
    assert "must be a JSON object" in errs("lead it", a, inv)


def test_ram_budget(ctx, analyse_text):
    a = analyse_text("MUSTER_TASK(cost = 1, ram = 2)\nvoid f() {}\n"
                     "MUSTER_TASK(cost = 1, ram = 1)\nvoid g() {}\n")
    _, inv, _ = ctx
    plan = {"lead": "esp32_a", "assignments": [{"task": "f", "boards": ["nano"]},
                                               {"task": "g", "boards": ["nano"]}]}
    assert "board 'nano' would need 3 KB RAM" in errs(plan, a, inv)


def test_message_size_against_smallest_board(ctx, analyse_text):
    _, inv, _ = ctx
    a = analyse_text("struct Big { float v[9]; };\n"            # 36 bytes > 34 for a Nano
                     "MUSTER_TASK(cost = 1)\nvoid f(Big b) { (void)b; }\n")
    plan = {"lead": "esp32_a", "assignments": [{"task": "f", "boards": ["esp32_a"]}]}
    assert "encode to 36 bytes; 'nano' carries at most 34" in errs(plan, a, inv)


def test_source_errors_block(ctx, analyse_text):
    _, inv, _ = ctx
    a = analyse_text("int g_count;\nMUSTER_TASK(cost = 1)\nint poll(int x) "
                     "{ g_count++; return x; }\nMUSTER_TASK(cost = 1)\nvoid h(uint8_t mg_x) "
                     "{ (void)mg_x; }\n")
    plan = {"lead": "esp32_a", "assignments": [{"task": "poll", "boards": ["esp32_a"]},
                                               {"task": "h", "boards": ["esp32_a"]}]}
    e = errs(plan, a, inv)
    for frag in ("is int; use", "touches global(s) g_count", "clashes with the generated",
                 "mg_ prefix is reserved"):
        assert frag in e


def test_avr_lead_node_limit(tmp_path, ctx):
    a, _, _ = ctx
    inv_file = tmp_path / "boards.yaml"
    inv_file.write_text(yaml.safe_dump({"boards": [{"id": f"n{i}", "type": "nanoatmega328"}
                                                   for i in range(5)]}))
    inv = P.load_inventory(inv_file)
    plan = {"lead": "n0", "assignments": [{"task": t["name"], "boards": ["n0"]}
                                          for t in a["tasks"]]}
    assert "tracks at most 4 boards" in errs(plan, a, inv)


def test_unknown_board_type(tmp_path):
    f = tmp_path / "b.yaml"
    f.write_text("boards:\n  - {id: x, type: stm32f4}\n")
    with pytest.raises(P.MusterError, match="never guesses pinouts"):
        P.load_inventory(f)

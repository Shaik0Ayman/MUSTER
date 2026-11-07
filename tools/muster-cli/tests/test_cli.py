# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
import json
import os
import subprocess
import sys

import pytest

from conftest import BOARDS, SOURCE
from muster_cli import pipeline as P
from muster_cli.cli import main


def test_plan_baseline(tmp_path, capsys):
    main(["plan", str(BOARDS), str(SOURCE), "--out", str(tmp_path), "--no-wiring", "--offline"])
    out = capsys.readouterr().out
    assert "0 error(s)" in out and "nano       holds sensor_read" in out
    assert (tmp_path / "include" / "muster_gen.h").is_file()


def test_plan_from_file(tmp_path, capsys):
    plan = P.baseline(BOARDS, SOURCE)
    plan["assignments"][0]["boards"] = ["esp32_b"]
    plan["rationale"] = "gait on b only"
    f = tmp_path / "my_plan.json"
    f.write_text(json.dumps(plan))
    main(["plan", str(BOARDS), str(SOURCE), "--plan", str(f), "--out", str(tmp_path / "o"),
          "--no-wiring"])
    out = capsys.readouterr().out
    assert "esp32_a    holds imu_filter, sensor_read" in out and "gait on b only" in out
    assert json.loads((tmp_path / "o" / "plan.json").read_text())["source"] == f"file:{f}"


def test_plan_rejected_exits(tmp_path, sketch):
    src = sketch("MUSTER_TASK(cost = 1)\nint f(int x) { return x; }\n")
    with pytest.raises(SystemExit) as e:
        main(["plan", str(BOARDS), str(src), "--out", str(tmp_path / "o"), "--no-wiring"])
    assert "plan rejected" in str(e.value)
    assert not (tmp_path / "o" / "platformio.ini").exists()


def test_analyze_json(capsys):
    main(["analyze", str(SOURCE), "--json"])
    assert [t["name"] for t in json.loads(capsys.readouterr().out)["tasks"]][0] == "gait_planner"


def test_boards_and_errors(capsys, tmp_path):
    main(["boards"])
    assert "nanoatmega328" in capsys.readouterr().out
    with pytest.raises(SystemExit, match="boards file not found"):
        main(["plan", str(tmp_path / "missing.yaml"), str(SOURCE)])


def test_deploy_dry_run(tmp_path, capsys):
    main(["plan", str(BOARDS), str(SOURCE), "--out", str(tmp_path), "--no-wiring"])
    capsys.readouterr()
    main(["deploy", str(BOARDS), "--out", str(tmp_path), "--dry-run"])
    lines = capsys.readouterr().out.splitlines()
    assert len(lines) == 3 and "-e esp32_a" in lines[-1] and all("(dry run)" in l for l in lines)


@pytest.mark.skipif(os.environ.get("MUSTER_PIO_TEST") != "1",
                    reason="set MUSTER_PIO_TEST=1 to compile for real boards (slow)")
def test_generated_project_compiles_for_every_board(tmp_path, capsys):
    main(["plan", str(BOARDS), str(SOURCE), "--out", str(tmp_path), "--no-wiring"])
    results = P.build(tmp_path)
    assert all(r["ok"] for r in results), results


def test_rejected_plan_prints_on_a_cp1252_console(tmp_path, sketch):
    # A Windows console is cp1252 by default; pytest's capture is UTF-8 and hid this.
    src = sketch("MUSTER_TASK(cost = 1)\nint f(int x) { return x; }\n")
    env = dict(os.environ, PYTHONIOENCODING="cp1252")
    r = subprocess.run([sys.executable, "-c", "from muster_cli.cli import main; main()",
                        "plan", str(BOARDS), str(src), "--out", str(tmp_path / "o"), "--no-wiring"],
                       capture_output=True, env=env)
    err = r.stderr.decode("cp1252", "replace")
    assert r.returncode == 1, err
    assert "Traceback" not in err and "plan rejected" in err
    assert b"error(s)" in r.stdout


@pytest.mark.parametrize("content, msg", [(None, "cannot read plan file"),
                                          ("{not json", "is not valid JSON")])
def test_bad_plan_file_is_a_clean_error(tmp_path, content, msg):
    f = tmp_path / "plan.json"
    if content is not None:
        f.write_text(content)
    with pytest.raises(SystemExit) as e:
        main(["plan", str(BOARDS), str(SOURCE), "--plan", str(f), "--out", str(tmp_path / "o"),
              "--no-wiring"])
    assert msg in str(e.value) and str(e.value).startswith("muster: ")

import json
import os

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

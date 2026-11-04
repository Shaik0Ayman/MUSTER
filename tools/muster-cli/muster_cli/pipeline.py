# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""The toolchain's operations, shared by the CLI (cli.py) and the MCP server
(mcp_server.py) so both behave identically:

  boards()      the board database
  analyse()     facts about a sketch                        (analyze.py)
  baseline()    the deterministic plan                      (partition.py)
  check()       validate any plan                           (validate.py)
  make()        re-validate, then generate the project      (pio.py, wiring.py)
  build()       compile every board's firmware              (pio run)
  upload()      flash every board, lead last; dry run unless told otherwise

make() validates again itself: a plan reaches code generation only through
the validator, however it got here (hard rule 8).
"""
import json
from pathlib import Path

import yaml

from . import analyze as _analyze
from . import pio as _pio
from .partition import baseline_partition
from .validate import validate
from .wiring import build_wiring

DB_PATH = Path(__file__).parent / "db" / "boards.yaml"


class MusterError(Exception):
    """A problem the user (or assistant) can fix: bad path, unknown board, ..."""


def load_db():
    return yaml.safe_load(DB_PATH.read_text(encoding="utf-8"))


def boards():
    """Board types muster knows, with the facts placement depends on."""
    db = load_db()
    out = []
    for key, s in db["boards"].items():
        can = s["can"]
        out.append({
            "type": key, "name": s["name"], "logic_v": s["logic_v"], "cores": s["cores"],
            "ram_kb": s["ram_kb"], "clock_mhz": s["clock_mhz"], "capabilities": s["flags"],
            "can": "on-chip TWAI + " + can["transceiver"] if can["native"] else can["module"],
            "max_args_bytes": s["runtime"]["max_msg"] - 6,
            "max_result_bytes": s["runtime"]["max_msg"] - 5,
            "max_nodes_as_lead": s["runtime"]["max_nodes"],
        })
    return out


def load_inventory(path, db=None):
    db = db or load_db()
    p = Path(path)
    if not p.is_file():
        raise MusterError(f"boards file not found: {path}")
    raw = yaml.safe_load(p.read_text(encoding="utf-8")) or {}
    if not isinstance(raw.get("boards"), list) or not raw["boards"]:
        raise MusterError(f"{path}: expected a 'boards:' list of {{id, type, port}}")
    inv = []
    shared = bool(raw.get("shared_images"))      # one image per board type; see images.py
    for entry in raw["boards"]:
        if not isinstance(entry, dict) or "id" not in entry or "type" not in entry:
            raise MusterError(f"{path}: every board needs an 'id' and a 'type'")
        if entry["type"] not in db["boards"]:
            raise MusterError(
                f"unknown board type '{entry['type']}'. Known: {', '.join(db['boards'])}. "
                f"Add it to the board database first; muster never guesses pinouts.")
        inv.append({**entry, "spec": db["boards"][entry["type"]], "shared_images": shared})
    return inv


def analyse(source):
    p = Path(source)
    if not p.is_file():
        raise MusterError(f"source file not found: {source}")
    try:
        return _analyze.analyze_file(p)
    except _analyze.AnalysisError as e:
        raise MusterError(str(e))


def baseline(boards_path, source):
    return baseline_partition(analyse(source), load_inventory(boards_path))


def check(plan, boards_path, source):
    analysis, inventory = analyse(source), load_inventory(boards_path)
    errors, warnings = validate(plan, analysis, inventory)
    return {"ok": not errors, "errors": errors, "warnings": warnings}


def make(plan, boards_path, source, out_dir, wiring=True):
    """Validate, then (only if it passes) write the project and plan.json."""
    db = load_db()
    analysis, inventory = analyse(source), load_inventory(boards_path, db)
    errors, warnings = validate(plan, analysis, inventory)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    record = {k: v for k, v in plan.items() if k != "validation"} if isinstance(plan, dict) else plan
    if isinstance(record, dict):
        record["validation"] = {"errors": errors, "warnings": warnings}
    (out / "plan.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    result = {"ok": not errors, "errors": errors, "warnings": warnings,
              "plan_json": str(out / "plan.json")}
    if errors:
        return result
    try:
        result["project"] = _pio.generate_project(plan, analysis, inventory, source, out)
    except _pio.GenerationError as e:
        raise MusterError(str(e))
    result["placement"] = {b["id"]: [a["task"] for a in plan["assignments"] if b["id"] in a["boards"]]
                           for b in inventory}
    result["lead"] = plan["lead"]
    if plan.get("modules"):
        result["modules"] = {m["module"]: m["boards"] for m in plan["modules"]}
    if wiring:
        result["wiring"] = build_wiring(inventory, db, out, warnings)
    return result


def _saved_plan(out_dir):
    p = Path(out_dir) / "plan.json"
    if not p.is_file():
        raise MusterError(f"{p} not found: run plan/generate first")
    plan = json.loads(p.read_text(encoding="utf-8"))
    if plan.get("validation", {}).get("errors"):
        raise MusterError(f"{p} failed validation; fix it and generate again")
    return plan


def build(out_dir, envs=None):
    if not (Path(out_dir) / "platformio.ini").is_file():
        raise MusterError(f"no platformio.ini in {out_dir}: generate the project first")
    try:
        return _pio.build(out_dir, envs)
    except _pio.GenerationError as e:
        raise MusterError(str(e))


def upload(boards_path, out_dir, dry_run=True):
    plan = _saved_plan(out_dir)
    try:
        return _pio.deploy(load_inventory(boards_path), out_dir, plan["lead"], dry_run=dry_run)
    except _pio.GenerationError as e:
        raise MusterError(str(e))

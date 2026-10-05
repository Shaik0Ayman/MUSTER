"""muster - split one program across many boards.

  muster plan   boards.yaml main.cpp   analyse, partition, validate, generate
  muster deploy                        build and upload every board
"""
import argparse
import json
import sys
from pathlib import Path
import yaml

from .analyze import analyze
from .llm import llm_partition, baseline_partition, ollama_available, DEFAULT_MODEL
from .validate import validate
from .pio import generate_project, deploy
from .wiring import build_wiring

DB_PATH = Path(__file__).parent / "db" / "boards.yaml"
MAX_REPAIRS = 3


def load_inventory(path, db):
    raw = yaml.safe_load(Path(path).read_text())
    inv = []
    for entry in raw["boards"]:
        if entry["type"] not in db["boards"]:
            known = ", ".join(db["boards"])
            sys.exit(f"unknown board type '{entry['type']}'. Known: {known}. "
                     f"Add it to the board database first; muster never guesses pinouts.")
        inv.append({**entry, "spec": db["boards"][entry["type"]]})
    return inv


def cmd_plan(args):
    db = yaml.safe_load(DB_PATH.read_text())
    inventory = load_inventory(args.boards, db)
    analysis = analyze(Path(args.source).read_text())
    out = Path(args.out)

    print(f"\n[1/4] analysed {args.source}: {len(analysis['tasks'])} task(s), "
          f"{len(analysis['globals'])} global(s)")
    for t in analysis["tasks"]:
        flags = []
        if t["pointer_params"]:
            flags.append("POINTER PARAMS")
        if t["globals_touched"]:
            flags.append("GLOBALS: " + ",".join(t["globals_touched"]))
        print(f"      - {t['name']:<16} needs={t['needs'] or '-'} ram={t['ram_kb']}KB "
              f"{'  ⚠ ' + '; '.join(flags) if flags else ''}")

    use_llm = not args.offline and ollama_available(args.model)
    print(f"[2/4] partitioning with {'LLM ' + args.model if use_llm else 'deterministic baseline'}")
    attempts, feedback = 0, None
    while True:
        attempts += 1
        plan = (llm_partition(analysis, inventory, args.model, feedback) if use_llm
                else baseline_partition(analysis, inventory))
        errors, warnings = validate(plan, analysis, inventory)
        if not errors or not use_llm or attempts > MAX_REPAIRS:
            break
        print(f"      attempt {attempts} rejected ({len(errors)} error(s)); asking for a repair")
        feedback = errors

    print(f"[3/4] validation: {len(errors)} error(s), {len(warnings)} warning(s) "
          f"after {attempts} attempt(s)")
    for e in errors:
        print(f"      ✗ {e}")
    for w in warnings:
        print(f"      ! {w}")

    out.mkdir(parents=True, exist_ok=True)
    plan["validation"] = {"errors": errors, "warnings": warnings, "attempts": attempts}
    (out / "plan.json").write_text(json.dumps(plan, indent=2))
    if errors:
        sys.exit("\nplan rejected. Fix the source issues above and re-run.")

    ini, resident = generate_project(plan, analysis, inventory, args.source, out)
    svg = build_wiring(inventory, db, out, warnings)
    print(f"[4/4] generated {ini}, wiring diagram {svg}")
    print(f"\n      lead: {plan['lead']}")
    for b in inventory:
        held = [a["task"] for a in plan["assignments"] if b["id"] in a["boards"]]
        print(f"      {b['id']:<10} holds {', '.join(held) or '(none)'}")
    print(f"\n      rationale: {plan['rationale']}\n")


def cmd_deploy(args):
    db = yaml.safe_load(DB_PATH.read_text())
    inventory = load_inventory(args.boards, db)
    plan = json.loads((Path(args.out) / "plan.json").read_text())
    for b in inventory:
        b["is_lead"] = b["id"] == plan["lead"]
    deploy(inventory, args.out, dry_run=args.dry_run)


def main(argv=None):
    p = argparse.ArgumentParser(prog="muster", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("plan", help="analyse, partition, validate and generate")
    pl.add_argument("boards")
    pl.add_argument("source")
    pl.add_argument("--out", default="muster_out")
    pl.add_argument("--model", default=DEFAULT_MODEL)
    pl.add_argument("--offline", action="store_true", help="skip the LLM, use the baseline")
    pl.set_defaults(fn=cmd_plan)

    dp = sub.add_parser("deploy", help="build and upload every board")
    dp.add_argument("boards")
    dp.add_argument("--out", default="muster_out")
    dp.add_argument("--dry-run", action="store_true")
    dp.set_defaults(fn=cmd_deploy)

    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main()

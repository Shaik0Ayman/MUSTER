"""muster - split one program across many boards.

  muster boards                           board types muster knows
  muster analyze main.cpp                 tasks, wire types, problems
  muster plan    boards.yaml main.cpp     validate a plan and generate the project
                 [--plan plan.json]         (default plan: the deterministic baseline)
  muster build   [-e ENV ...]             compile every board's firmware
  muster deploy  boards.yaml [--dry-run]  upload every board, lead last
  muster mcp                              serve these tools to an MCP client
                                          (e.g. Claude Code) over stdio
"""
import argparse
import json
import sys
from pathlib import Path

from . import pipeline as P


def _print_tasks(analysis, source):
    print(f"\n[1/4] analysed {source}: {len(analysis['tasks'])} task(s)")
    for t in analysis["tasks"]:
        size = (f"{t['args_bytes']} B in / {t['ret_bytes']} B out"
                if t["args_bytes"] is not None else "not sendable")
        print(f"      - {t['name']:<16} needs={'|'.join(t['needs']) or '-'} cost={t['cost']} "
              f"ram={t['ram_kb']}KB  {size}")
        for e in t["errors"]:
            print(f"          ✗ {e}")
    if analysis["stubbed_headers"]:
        print(f"      (not found, treated as empty: {', '.join(analysis['stubbed_headers'])})")


def cmd_boards(args):
    for b in P.boards():
        print(f"{b['type']:<22} {b['name']:<28} {b['cores']} core(s) {b['clock_mhz']:>4} MHz "
              f"{b['ram_kb']:>4} KB  {'|'.join(b['capabilities']) or '-':<14} {b['logic_v']} V  "
              f"CAN: {b['can']}")


def cmd_analyze(args):
    a = P.analyse(args.source)
    if args.json:
        print(json.dumps(a, indent=2))
    else:
        _print_tasks(a, args.source)


def cmd_plan(args):
    analysis = P.analyse(args.source)
    _print_tasks(analysis, args.source)
    if args.plan:
        plan = json.loads(Path(args.plan).read_text(encoding="utf-8"))
        plan["source"] = f"file:{args.plan}"         # who submitted it, not who drafted it
        print(f"[2/4] plan from {args.plan}")
    else:
        plan = P.baseline(args.boards, args.source)
        print("[2/4] plan from the deterministic baseline")

    r = P.make(plan, args.boards, args.source, args.out, wiring=not args.no_wiring)
    print(f"[3/4] validation: {len(r['errors'])} error(s), {len(r['warnings'])} warning(s)")
    for e in r["errors"]:
        print(f"      ✗ {e}")
    for w in r["warnings"]:
        print(f"      ! {w}")
    if not r["ok"]:
        sys.exit("\nplan rejected. Fix the issues above and re-run.")

    w = r.get("wiring", {})
    print(f"[4/4] generated {r['project']['platformio_ini']} "
          f"({len(r['project']['envs'])} envs)"
          + (f", wiring {w['svg']}" if w.get("rendered") else ""))
    if w.get("skipped"):
        print(f"      ! {w['skipped']}")
    print(f"\n      lead: {r['lead']}")
    for bid, held in r["placement"].items():
        print(f"      {bid:<10} holds {', '.join(held) or '(none)'}")
    print(f"\n      rationale: {plan.get('rationale', '-')}")
    print(f"      next: muster build --out {args.out}\n")


def cmd_build(args):
    results = P.build(args.out, args.env)
    for res in results:
        print(f"== {res['env']}: {'OK' if res['ok'] else 'FAILED'}")
        for line in res["summary"]:
            print(f"   {line}")
    if not all(res["ok"] for res in results):
        sys.exit(1)


def cmd_deploy(args):
    for s in P.upload(args.boards, args.out, dry_run=args.dry_run):
        print(f"  $ {s['command']}" + ("" if s["ran"] else "   (dry run)"))
        if s["ran"] and not s["ok"]:
            print("\n".join(f"    {l}" for l in s["tail"]))
            sys.exit(f"upload failed for {s['board']}; stopped so the bus is not left half-flashed")


def cmd_mcp(args):
    from .mcp_server import serve
    serve()


def main(argv=None):
    p = argparse.ArgumentParser(prog="muster", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("boards", help="list board types").set_defaults(fn=cmd_boards)

    an = sub.add_parser("analyze", help="show tasks and their wire types")
    an.add_argument("source")
    an.add_argument("--json", action="store_true")
    an.set_defaults(fn=cmd_analyze)

    pl = sub.add_parser("plan", help="validate a plan and generate the project")
    pl.add_argument("boards")
    pl.add_argument("source")
    pl.add_argument("--plan", help="plan.json to use instead of the baseline")
    pl.add_argument("--out", default="muster_out")
    pl.add_argument("--no-wiring", action="store_true", help="skip the wiring diagram")
    pl.add_argument("--offline", action="store_true", help=argparse.SUPPRESS)  # pre-MCP flag
    pl.set_defaults(fn=cmd_plan)

    bd = sub.add_parser("build", help="compile every board (pio run)")
    bd.add_argument("--out", default="muster_out")
    bd.add_argument("-e", "--env", action="append", help="only this env (repeatable)")
    bd.set_defaults(fn=cmd_build)

    dp = sub.add_parser("deploy", help="upload every board, lead last")
    dp.add_argument("boards")
    dp.add_argument("--out", default="muster_out")
    dp.add_argument("--dry-run", action="store_true")
    dp.set_defaults(fn=cmd_deploy)

    sub.add_parser("mcp", help="run the MCP server on stdio").set_defaults(fn=cmd_mcp)

    args = p.parse_args(argv)
    try:
        args.fn(args)
    except P.MusterError as e:
        sys.exit(f"muster: {e}")


if __name__ == "__main__":
    main()

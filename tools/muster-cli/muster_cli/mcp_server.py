# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""MCP server: the muster toolchain as tools for an MCP client (Claude Code,
Claude Desktop, or any other assistant). The assistant does the partitioning
judgement the local model used to do; muster supplies facts, checks and
deterministic generation, and the validator still has the final word.

Transport: stdio, JSON-RPC 2.0, one message per line (MCP "stdio" transport).
Hand-written to keep the toolchain dependency-free; it implements initialize,
ping, tools/list and tools/call, which is all a tools-only server needs.
stdout carries protocol messages only: everything else goes to stderr.

Run:  muster mcp            (or: python -m muster_cli.mcp_server)
"""
import contextlib
import json
import sys
import traceback
from pathlib import Path

from . import pipeline as P

SERVER_INFO = {"name": "muster", "version": "0.2.0"}
PROTOCOL_VERSIONS = ["2025-06-18", "2025-03-26", "2024-11-05"]   # newest first

INSTRUCTIONS = """\
muster splits one embedded C++ program (Arduino style) across several
microcontroller boards that share a CAN bus. Functions marked
MUSTER_TASK(needs=..., cost=..., ram=...) may run on any board that holds
them; a lead board places each call at run time.

You decide the partition: which boards hold each task's code, and which board
leads. muster supplies the facts and the checks. Typical flow:
 1. list_boards, and read the user's boards.yaml (their bench inventory).
 2. analyze_source on their sketch. Fix every task error first by editing
    the sketch (e.g. int -> int16_t, pass values not pointers); no plan can
    fix those. Re-analyse until clean.
 3. baseline_plan gives a valid starting point. Improve it where you can
    justify it: put each task on 2+ boards when they qualify (failover);
    keep heavy tasks (high cost or ram) off small boards; lead = the board
    with the most RAM and cores; mind the per-board RAM budget.
 4. validate_plan until ok is true. Errors are binding; warnings are advice
    to relay to the user.
 5. generate_project (validates again itself), then build_project to
    compile every board.
 6. deploy only when the user asks to flash, with confirm=true; without it
    deploy is a dry run.
Never invent boards, tasks or pins: pins come only from muster's board
database, and plans contain no pins. Paths are relative to the server's
working directory unless absolute.
Plan shape: {"lead": board_id, "assignments": [{"task": name, "boards":
[board_id, ...]}], "modules": [{"module": name, "boards": [candidate ids],
"count": n}], "rationale": "two or three sentences"}
"modules" is only for programs with MUSTER_MODULE functions (long-running
units, int8_t name(uint8_t event)). "boards" lists the boards that MAY run the
module; the lead picks "count" of the live ones at run time and moves a module
if its board is lost. With "shared_images: true" in boards.yaml all boards of a
type that can read a chip ID share one image, so they all hold the union of the
tasks and modules given to any of them."""

PLAN_SCHEMA = {
    "type": "object",
    "properties": {
        "lead": {"type": "string", "description": "board id that coordinates the bus"},
        "assignments": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "task": {"type": "string"},
                    "boards": {"type": "array", "items": {"type": "string"}},
                },
                "required": ["task", "boards"],
            },
        },
        "modules": {
            "type": "array",
            "items": {
                "type": "object",
                "properties": {
                    "module": {"type": "string"},
                    "boards": {"type": "array", "items": {"type": "string"}},
                    "count": {"type": "integer", "minimum": 1},
                },
                "required": ["module", "boards"],
            },
        },
        "rationale": {"type": "string"},
    },
    "required": ["lead", "assignments"],
}
BOARDS = {"type": "string", "description": "path to the inventory boards.yaml"}
SOURCE = {"type": "string", "description": "path to the sketch (main.cpp)"}
OUT = {"type": "string", "description": "project directory to write", "default": "muster_out"}


def _plan(args):
    plan = args["plan"]
    if isinstance(plan, str):                     # some clients send JSON text
        plan = json.loads(plan)
    if isinstance(plan, dict):
        plan["source"] = "mcp"                       # who submitted it, not who drafted it
    return plan


def _out(args):
    return str(Path(args.get("out_dir") or "muster_out").resolve())


def t_list_boards(args):
    return {"boards": P.boards()}


def t_analyze(args):
    return P.analyse(args["source"])


def t_baseline(args):
    return P.baseline(args["boards"], args["source"])


def t_validate(args):
    return P.check(_plan(args), args["boards"], args["source"])


def t_generate(args):
    return P.make(_plan(args), args["boards"], args["source"], _out(args),
                  wiring=args.get("wiring", True))


def t_wiring(args):
    from .wiring import build_wiring
    return build_wiring(P.load_inventory(args["boards"]), P.load_db(), _out(args))


def t_build(args):
    results = P.build(_out(args), args.get("envs"))
    return {"ok": all(r["ok"] for r in results), "results": results}


def t_deploy(args):
    confirm = args.get("confirm") is True
    steps = P.upload(args["boards"], _out(args), dry_run=not confirm)
    return {"dry_run": not confirm, "steps": steps,
            "ok": all(s.get("ok", True) for s in steps)}


RO = {"readOnlyHint": True, "openWorldHint": False}
TOOLS = [
    ("list_boards", t_list_boards, "Board types muster knows: capabilities, RAM, clock, "
     "CAN hardware, and the largest call arguments/result each can carry.", {}, [], RO),
    ("analyze_source", t_analyze, "Analyse a sketch: every MUSTER_TASK with its needs, cost, "
     "RAM, wire types and encoded sizes, plus errors that must be fixed in the source "
     "(non-portable types, pointers, globals).", {"source": SOURCE}, ["source"], RO),
    ("baseline_plan", t_baseline, "The deterministic plan: each task on every board that "
     "qualifies, the board with the most RAM leads. A valid starting point to improve on.",
     {"boards": BOARDS, "source": SOURCE}, ["boards", "source"], RO),
    ("validate_plan", t_validate, "Check a plan against the sketch and inventory. Returns "
     "ok, errors (binding) and warnings. Use before generate_project.",
     {"boards": BOARDS, "source": SOURCE, "plan": PLAN_SCHEMA}, ["boards", "source", "plan"], RO),
    ("generate_project", t_generate, "Validate the plan again and, only if it passes, write "
     "the PlatformIO project: typed trampolines and call stubs (muster_gen.h), one env per "
     "board, plan.json and wiring notes. Returns what was written.",
     {"boards": BOARDS, "source": SOURCE, "plan": PLAN_SCHEMA, "out_dir": OUT,
      "wiring": {"type": "boolean", "default": True}}, ["boards", "source", "plan"],
     {"readOnlyHint": False, "destructiveHint": False, "idempotentHint": True,
      "openWorldHint": False}),
    ("render_wiring", t_wiring, "Write the wiring diagram (wiring.yaml, and SVG/PNG if "
     "Graphviz is installed) and WIRING_NOTES.txt for an inventory. Pins come from the "
     "board database only.", {"boards": BOARDS, "out_dir": OUT}, ["boards"],
     {"readOnlyHint": False, "destructiveHint": False, "idempotentHint": True,
      "openWorldHint": False}),
    ("build_project", t_build, "Compile every board's firmware with PlatformIO (pio run). "
     "No hardware needed. Can take minutes the first time.",
     {"out_dir": OUT, "envs": {"type": "array", "items": {"type": "string"},
                               "description": "only these envs (default: all)"}}, [],
     {"readOnlyHint": False, "destructiveHint": False, "idempotentHint": True,
      "openWorldHint": False}),
    ("deploy", t_deploy, "Upload every board, lead last. A DRY RUN (commands only) unless "
     "confirm is true. Only flash when the user has asked to.",
     {"boards": BOARDS, "out_dir": OUT,
      "confirm": {"type": "boolean", "default": False,
                  "description": "true = really flash the boards"}}, ["boards"],
     {"readOnlyHint": False, "destructiveHint": True, "idempotentHint": False,
      "openWorldHint": True}),
]
HANDLERS = {name: fn for name, fn, *_ in TOOLS}


def tool_list():
    return [{"name": name, "description": desc,
             "inputSchema": {"type": "object", "properties": props, "required": req},
             "annotations": ann}
            for name, _fn, desc, props, req, ann in TOOLS]


def call_tool(name, args):
    """Run one tool. Problems the caller can fix come back as isError results."""
    if name not in HANDLERS:
        return {"content": [{"type": "text", "text": f"unknown tool '{name}'"}], "isError": True}
    spec = next(t for t in TOOLS if t[0] == name)
    missing = [k for k in spec[4] if k not in (args or {})]
    if missing:
        return {"content": [{"type": "text", "text": f"missing argument(s): {', '.join(missing)}"}],
                "isError": True}
    try:
        with contextlib.redirect_stdout(sys.stderr):     # keep stdout for protocol only
            result = HANDLERS[name](args or {})
        text, is_error = json.dumps(result, indent=2), False
    except (P.MusterError, ValueError, KeyError, TypeError) as e:
        text, is_error, result = f"{type(e).__name__}: {e}", True, None
    except Exception as e:                               # a bug: report, keep serving
        traceback.print_exc(file=sys.stderr)
        text, is_error, result = f"internal error: {type(e).__name__}: {e}", True, None
    out = {"content": [{"type": "text", "text": text}], "isError": is_error}
    if isinstance(result, dict):
        out["structuredContent"] = result
    return out


class Server:
    def __init__(self):
        self.version = PROTOCOL_VERSIONS[0]

    def handle(self, msg):
        """One JSON-RPC message in, one response (or None for notifications) out."""
        if not isinstance(msg, dict) or msg.get("jsonrpc") != "2.0":
            return _error(None, -32600, "invalid request")
        mid, method, params = msg.get("id"), msg.get("method"), msg.get("params") or {}
        if method is None:                               # a response to us: ignore
            return None
        if "id" not in msg:                              # notification
            return None
        if method == "initialize":
            asked = params.get("protocolVersion")
            self.version = asked if asked in PROTOCOL_VERSIONS else PROTOCOL_VERSIONS[0]
            return _result(mid, {"protocolVersion": self.version,
                                 "capabilities": {"tools": {"listChanged": False}},
                                 "serverInfo": SERVER_INFO,
                                 "instructions": INSTRUCTIONS})
        if method == "ping":
            return _result(mid, {})
        if method == "tools/list":
            return _result(mid, {"tools": tool_list()})
        if method == "tools/call":
            if not isinstance(params.get("name"), str):
                return _error(mid, -32602, "tools/call needs a tool name")
            res = call_tool(params["name"], params.get("arguments") or {})
            if self.version < "2025-06-18":
                res.pop("structuredContent", None)
            return _result(mid, res)
        return _error(mid, -32601, f"method not found: {method}")


def _result(mid, result):
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def _error(mid, code, message):
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}


def serve(stdin=None, stdout=None):
    stdin = stdin or sys.stdin.buffer
    stdout = stdout or sys.stdout.buffer
    server = Server()
    print(f"muster MCP server {SERVER_INFO['version']} on stdio", file=sys.stderr)
    for raw in stdin:
        line = raw.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            replies = [_error(None, -32700, "parse error")]
        else:
            batch = msg if isinstance(msg, list) else [msg]
            replies = [r for r in (server.handle(m) for m in batch) if r is not None]
        for r in replies:
            stdout.write(json.dumps(r, separators=(",", ":")).encode("utf-8") + b"\n")
        stdout.flush()


if __name__ == "__main__":
    serve()

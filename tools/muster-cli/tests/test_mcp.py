# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
import json
import subprocess
import sys

import pytest

from conftest import BOARDS, ROOT, SOURCE
from muster_cli.mcp_server import PROTOCOL_VERSIONS, Server


def rpc(server, method, params=None, mid=1):
    msg = {"jsonrpc": "2.0", "id": mid, "method": method}
    if params is not None:
        msg["params"] = params
    return server.handle(msg)


def call(server, name, **args):
    r = rpc(server, "tools/call", {"name": name, "arguments": args})["result"]
    return r, (json.loads(r["content"][0]["text"]) if not r["isError"] else r["content"][0]["text"])


@pytest.fixture
def server():
    s = Server()
    rpc(s, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                          "clientInfo": {"name": "test", "version": "0"}})
    return s


def test_initialize_negotiates_version():
    s = Server()
    r = rpc(s, "initialize", {"protocolVersion": "2024-11-05"})["result"]
    assert r["protocolVersion"] == "2024-11-05"
    assert r["capabilities"] == {"tools": {"listChanged": False}}
    assert r["serverInfo"]["name"] == "muster" and "validate_plan" in r["instructions"]
    r = rpc(Server(), "initialize", {"protocolVersion": "1999-01-01"})["result"]
    assert r["protocolVersion"] == PROTOCOL_VERSIONS[0]


def test_protocol_basics(server):
    assert rpc(server, "ping")["result"] == {}
    assert rpc(server, "resources/list")["error"]["code"] == -32601
    assert server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}) is None
    assert server.handle({"id": 1, "method": "ping"})["error"]["code"] == -32600
    assert rpc(server, "tools/call", {})["error"]["code"] == -32602


def test_tools_list(server):
    tools = {t["name"]: t for t in rpc(server, "tools/list")["result"]["tools"]}
    assert set(tools) == {"list_boards", "analyze_source", "baseline_plan", "validate_plan",
                          "generate_project", "render_wiring", "build_project", "deploy"}
    for t in tools.values():
        assert t["inputSchema"]["type"] == "object" and t["description"]
    assert tools["deploy"]["annotations"]["destructiveHint"] is True
    assert tools["validate_plan"]["annotations"]["readOnlyHint"] is True
    assert tools["validate_plan"]["inputSchema"]["required"] == ["boards", "source", "plan"]


def test_assistant_flow(server, tmp_path):
    b, s = str(BOARDS), str(SOURCE)
    _, boards = call(server, "list_boards")
    assert {x["type"] for x in boards["boards"]} == {"esp32-s3-devkitc-1", "nanoatmega328"}
    _, analysis = call(server, "analyze_source", source=s)
    assert [t["name"] for t in analysis["tasks"]] == ["gait_planner", "imu_filter", "sensor_read"]
    _, plan = call(server, "baseline_plan", boards=b, source=s)

    bad = dict(plan, lead="ghost")
    _, v = call(server, "validate_plan", boards=b, source=s, plan=bad)
    assert v["ok"] is False and any("ghost" in e for e in v["errors"])
    _, g = call(server, "generate_project", boards=b, source=s, plan=bad, out_dir=str(tmp_path / "x"))
    assert g["ok"] is False and not (tmp_path / "x" / "platformio.ini").exists()

    _, v = call(server, "validate_plan", boards=b, source=s, plan=plan)
    assert v["ok"] is True
    out = str(tmp_path / "proj")
    r, g = call(server, "generate_project", boards=b, source=s, plan=json.dumps(plan),
                out_dir=out, wiring=False)
    assert g["ok"] and (tmp_path / "proj" / "include" / "muster_gen.h").is_file()
    assert r["structuredContent"]["ok"] is True
    saved = json.loads((tmp_path / "proj" / "plan.json").read_text())
    assert saved["source"] == "mcp" and saved["validation"]["errors"] == []

    _, d = call(server, "deploy", boards=b, out_dir=out)
    assert d["dry_run"] is True and [x["board"] for x in d["steps"]][-1] == "esp32_a"
    assert all(not x["ran"] for x in d["steps"])


def test_tool_errors_are_results_not_crashes(server, tmp_path):
    r, text = call(server, "analyze_source", source=str(tmp_path / "nope.cpp"))
    assert r["isError"] and "not found" in text
    r, text = call(server, "validate_plan", boards=str(BOARDS))
    assert r["isError"] and "missing argument(s): source, plan" in text
    r, text = call(server, "no_such_tool")
    assert r["isError"]
    r, text = call(server, "deploy", boards=str(BOARDS), out_dir=str(tmp_path))
    assert r["isError"] and "plan.json" in text


def test_structured_content_only_for_new_clients():
    s = Server()
    rpc(s, "initialize", {"protocolVersion": "2025-03-26"})
    r = rpc(s, "tools/call", {"name": "list_boards", "arguments": {}})["result"]
    assert "structuredContent" not in r and not r["isError"]


def test_stdio_transport():
    """The real server over pipes: only JSON-RPC on stdout, logs on stderr."""
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                    "clientInfo": {"name": "t", "version": "0"}}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
         "params": {"name": "baseline_plan", "arguments": {"boards": str(BOARDS), "source": str(SOURCE)}}},
    ]
    stdin = "".join(json.dumps(m) + "\n" for m in msgs) + "this is not json\n"
    p = subprocess.run([sys.executable, "-m", "muster_cli.mcp_server"], input=stdin,
                       capture_output=True, text=True, cwd=ROOT, timeout=120)
    lines = [json.loads(l) for l in p.stdout.splitlines()]          # every line is JSON
    assert [m.get("id") for m in lines] == [1, 2, 3, None]
    assert lines[2]["result"]["isError"] is False
    assert lines[3]["error"]["code"] == -32700
    assert "muster MCP server" in p.stderr

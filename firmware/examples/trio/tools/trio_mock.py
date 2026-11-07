#!/usr/bin/env python3
"""Preview the trio dashboard without any hardware.

Serves the exact page embedded in muster_trio/dashboard.h and fakes the lead's
/api/state JSON (same fields as trio_lead.h's serve_state). Worker B drops out
for ~10 s every 40 s so you can see failover. The numbers are SIMULATED - this
shows what the page looks like, not how the boards behave.

    python3 tools/trio_mock.py            # then open http://localhost:8080/
"""
import json, math, re, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

HTML = re.search(r'R"TRIOHTML\((.*)\)TRIOHTML"',
                 (Path(__file__).parent.parent / "muster_trio" / "dashboard.h").read_text(), re.S).group(1)
T0 = time.time()
BOARDS = [  # name, lead, node, uid, chip, cpu, heap, base busy us
    ("Lead", 1, 0, "A1B2C3D4", 2, 160, 212000, 9800),
    ("Worker A", 0, 87, "3C71A0F2", 1, 240, 231000, 780),
    ("Worker B", 0, 142, "3C71B9E8", 1, 240, 229000, 790),
]
state = {"ok": 0, "calls": [0, 0, 0], "tasks": [0, 0, 0], "last": [0, 0, 0], "log": [], "b_up": True}


def say(t, m):
    state["log"] = (state["log"] + [{"t": int(t * 1000), "m": m}])[-10:]


def snapshot():
    t = time.time() - T0
    b_up = (t % 40) < 30
    if b_up != state["b_up"]:
        state["b_up"] = b_up
        say(t, "worker joined: node 142" if b_up else "worker LOST: node 142 (calls move to the others)")
    steps = int(t * 16)
    new = steps - state["ok"]
    for _ in range(max(new, 0)):
        up = [i for i in (1, 2) if i == 1 or b_up]
        i = up[state["ok"] % len(up)]
        state["calls"][i] += 1
        state["tasks"][i] += 1
        state["last"][i] = state["ok"]
        state["ok"] += 1
    boards = []
    for i, (name, lead, node, uid, chip, cpu, heap, busy) in enumerate(BOARDS):
        step = max(state["ok"] - 1, 0) if lead else state["last"][i]   # the lead mirrors the latest answer
        x = 2 * math.pi * (step % 128) / 128
        val = (math.pi - x) / 2
        duty = max(0, min(255, round((val + math.pi / 2) / math.pi * 255)))
        alive = 1 if (i != 2 or b_up) else 0
        boards.append({
            "slot": i, "name": name, "lead": lead, "node": node, "uid": uid, "chip": chip, "alive": alive,
            "cpu": cpu, "heap": heap - int(2000 * math.sin(t / 7 + i)), "up": int(t * 1000) + 5000 * i,
            "tasks": state["tasks"][i], "calls": state["calls"][i], "duty": duty, "val": round(val, 3),
            "busy": busy + int(40 * math.sin(t * 3 + i)), "avg": busy, "age": 60 if state["calls"][i] else -1,
            "inflight": 1 if alive and not lead else 0, "queued": 50 if alive and not lead else 0})
    return {"t": int(t * 1000), "ch": 1, "ssid": "muster-trio", "alive": sum(b["alive"] for b in boards),
            "ok": state["ok"], "failed": 0, "cps": 16.0, "step": steps,
            "radio": {"sent": state["ok"] * 5, "txfail": 0, "rxdrop": 0}, "boards": boards, "log": state["log"]}


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/state":
            body, ctype = json.dumps(snapshot()).encode(), "application/json"
        elif self.path == "/":
            body, ctype = HTML.encode(), "text/html; charset=utf-8"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    say(0, "worker joined: node 87"); say(0.2, "worker joined: node 142")
    print(f"trio dashboard preview (simulated data): http://localhost:{port}/")
    HTTPServer(("0.0.0.0", port), H).serve_forever()

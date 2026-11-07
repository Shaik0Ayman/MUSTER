# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""Telemetry frames (muster_cli.telemetry) and the PC-hosted dashboard."""
import threading
import urllib.request

import pytest

from conftest import FIRMWARE_SRC, host_cxx
from muster_cli import telemetry as T
from muster_cli.cli import main

FIRMWARE_TEST = FIRMWARE_SRC.parent / "test"


def test_demo_frames_round_trip():
    sim = T.DemoCluster()
    last = None
    for _ in range(200):                         # 50 s: includes a worker dropping out
        f = sim.step()
        s = T.decode(f)
        assert T.encode(s) == f                  # encode(decode(x)) is the identity
        last = s
    assert last["seq"] == 200 and len(last["nodes"]) == 5 and len(last["links"]) == 2
    assert sum(last["hist"]) == sum(n["ok"] + n["failed"] for n in last["nodes"])
    assert {e["name"] for e in last["events"]} <= set(T.EVENTS.values())


def test_decode_rejects_damage():
    f = bytearray(T.DemoCluster().step())
    for bad, why in [(f[:-1], "length"), (b"XY" + f[2:], "not a Muster"),
                     (f[:10] + bytes([f[10] ^ 1]) + f[11:], "CRC")]:
        with pytest.raises(T.FrameError, match=why):
            T.decode(bytes(bad))


def test_scanner_finds_frames_between_text_and_noise():
    sim = T.DemoCluster()
    frames = [sim.step() for _ in range(5)]
    stream = (b"boot... MTX not a frame\r\n" + frames[0] + b"[ 1234] worker joined\n" + frames[1]
              + b"\x00\xffMT\x01\xff\xff" + frames[2] + frames[3] + b"MT" + frames[4])
    sc = T.FrameScanner()
    got = []
    for i in range(0, len(stream), 3):           # arrives in tiny chunks, as from a UART
        got += list(sc.feed(stream[i:i + 3]))
    assert [g["seq"] for g in got] == [1, 2, 3, 4, 5]


def test_frame_written_by_the_c_runtime_decodes(tmp_path):
    """The real encoder: firmware/test/test_telemetry.c built on the host writes one frame."""
    run = host_cxx()
    if run is None:
        pytest.skip("no host C++ compiler (set MUSTER_HOST_CXX, or MUSTER_HOST_CXX=wsl)")
    src = tmp_path / "tel.cpp"                   # built as C++, as Arduino builds it
    src.write_bytes((FIRMWARE_TEST / "test_telemetry.c").read_bytes())
    r = run(tmp_path, ["c++", "-std=c++11", "-O1", "-I", str(FIRMWARE_SRC), "-I", str(FIRMWARE_TEST),
                       str(src), "-o", str(tmp_path / "tel")])
    assert r.returncode == 0, r.stderr
    r = run(tmp_path, ["./tel", "frame.bin"])
    assert r.returncode == 0 and "ALL TESTS PASSED" in r.stdout, r.stdout
    s = T.decode((tmp_path / "frame.bin").read_bytes())
    ids = sorted(n["id"] for n in s["nodes"])
    assert ids == [0, 1, 2] and s["lead"] == 0 and s["has_us"]
    assert s["ok"] == 23 and s["failed"] >= 0 and s["retries"] >= 1
    assert sum(n["ok"] for n in s["nodes"]) >= 23
    assert any(e["name"] == "JOINED" or e["name"] == "DONE" for e in s["events"])
    w1 = next(n for n in s["nodes"] if n["id"] == 1)
    assert w1["lat_min"] <= w1["lat_avg"] <= w1["lat_max"] and w1["busy_us"] > 0


def test_dashboard_server_serves_page_and_frames():
    from http.server import ThreadingHTTPServer
    from muster_cli.dashboard import Latest, make_handler
    latest = Latest("test source")
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(latest))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{httpd.server_address[1]}"
    try:
        page = urllib.request.urlopen(base + "/").read().decode()
        assert "<title>Muster Dashboard</title>" in page and "api/frame" in page
        r = urllib.request.urlopen(base + "/api/frame")
        assert r.status == 204 and r.headers["X-Muster-Source"] == "test source"
        f = T.DemoCluster().step()
        latest.put(f)
        r = urllib.request.urlopen(base + "/api/frame")
        assert r.status == 200 and r.read() == f
    finally:
        httpd.shutdown()
        httpd.server_close()


def test_dash_needs_a_source():
    with pytest.raises(SystemExit) as e:
        main(["dash"])
    assert "--serial" in str(e.value)

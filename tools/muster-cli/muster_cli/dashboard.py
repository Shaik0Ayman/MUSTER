# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""`muster dash`: the Muster dashboard, hosted on your PC.

The lead only writes telemetry frames to its serial port (MUSTER_TELEMETRY=1 and
muster_telemetry_frame()); this process finds the frames in the stream and serves
the dashboard page, which decodes them in the browser. Nothing is hosted on the
boards, so they spend no RAM or CPU on a web server.

Sources: --serial PORT (needs pyserial), --replay FILE (raw bytes captured from
the port), or --demo (a simulated cluster, to try the page with no hardware).
Stdlib only, apart from pyserial for --serial.
"""
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import telemetry as T

PAGE = Path(__file__).parent / "dash" / "index.html"


class Latest:
    """The newest frame from the source, shared with the HTTP threads."""

    def __init__(self, label):
        self.label = label
        self.frame = None
        self.frames = 0
        self.bad = 0
        self.lock = threading.Lock()

    def put(self, frame_bytes):
        with self.lock:
            self.frame = frame_bytes
            self.frames += 1

    def get(self):
        with self.lock:
            return self.frame


def _pump(stream_read, latest, log, stop):
    """Read bytes, keep the newest valid frame; text from the sketch goes to the log."""
    scan = T.FrameScanner()
    while not stop.is_set():
        data = stream_read()
        if data is None:                  # end of a replay
            return
        if not data:
            continue
        for snap in scan.feed(data):
            latest.put(T.encode(snap))    # re-encode = a canonical, verified frame
        latest.bad = scan.bad
        if log:
            text = bytes(b for b in data if b in (9, 10, 13) or 32 <= b < 127)
            if text.strip():
                log.write(text.decode("ascii", "replace"))
                log.flush()


def serial_source(port, baud, latest, log, stop):
    try:
        import serial  # pyserial
    except ImportError:
        raise SystemExit("muster dash --serial needs pyserial: pip install pyserial "
                         "(or pip install -e \"tools/muster-cli[dash]\")") from None
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
    except (OSError, serial.SerialException) as e:
        raise SystemExit(f"muster dash: cannot open {port}: {e}") from None
    threading.Thread(target=_pump, args=(lambda: ser.read(4096), latest, log, stop),
                     daemon=True).start()


def replay_source(path, latest, stop, rate_hz=4):
    data = Path(path).read_bytes()
    chunks = [data[i:i + 256] for i in range(0, len(data), 256)]

    def read():
        if not chunks:
            return None
        time.sleep(1 / rate_hz / 4)
        return chunks.pop(0)
    threading.Thread(target=_pump, args=(read, latest, None, stop), daemon=True).start()


def demo_source(latest, stop, period=0.25):
    sim = T.DemoCluster()

    def run():
        try:
            while not stop.is_set():
                latest.put(sim.step(int(period * 1000)))
                time.sleep(period)
        except Exception as e:            # a dead thread would leave the page "waiting" forever
            latest.label = f"demo stopped: {e!r}"
            raise
    threading.Thread(target=run, daemon=True).start()


def make_handler(latest):
    page = PAGE.read_bytes()

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):        # keep the console for the board's own output
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Muster-Source", latest.label)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            if path in ("/", "/index.html"):
                self._send(200, page, "text/html; charset=utf-8")
            elif path == "/api/frame":
                f = latest.get()
                if f is None:
                    self._send(204, b"", "application/octet-stream")
                else:
                    self._send(200, f, "application/octet-stream")
            else:
                self._send(404, b"not found", "text/plain")
    return H


def serve(args):
    stop = threading.Event()
    if args.demo:
        latest = Latest("demo (simulated)")
        demo_source(latest, stop)
    elif args.replay:
        latest = Latest(f"replay {Path(args.replay).name}")
        replay_source(args.replay, latest, stop)
    else:
        latest = Latest(f"serial {args.serial} @ {args.baud}")
        serial_source(args.serial, args.baud, latest, sys.stdout if args.echo else None, stop)
    try:
        httpd = ThreadingHTTPServer((args.host, args.port), make_handler(latest))
    except OSError as e:
        stop.set()
        raise SystemExit(f"muster dash: cannot listen on {args.host}:{args.port}: {e} "
                         "(try another --port)") from None
    url = f"http://{'localhost' if args.host in ('127.0.0.1', '0.0.0.0') else args.host}:{args.port}/"
    print(f"Muster dashboard: {url}   (source: {latest.label}; Ctrl+C to stop)", file=sys.stderr)
    if args.open:
        import webbrowser
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        httpd.server_close()

# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""Muster telemetry frames: decode (from a serial stream), encode (for the demo),
and a small cluster simulator for `muster dash --demo`.

The frame is produced by muster_telemetry_frame() in firmware/src/muster.h; its
layout is documented in docs/TELEMETRY.md. Everything is fixed-width little-endian.
"""
import math
import random
import struct

VERSION = 1
MAGIC = b"MT"
EVENTS = {1: "JOINED", 2: "LOST", 3: "PLACED", 4: "DONE", 5: "RETRY", 6: "ACCEPTED",
          7: "VERSION", 8: "REFUSED", 9: "MODULE_START", 10: "MODULE_STOP", 11: "IDCLASH"}
HIST_EDGES_US = [500, 1000, 2000, 5000, 10000, 20000, 50000]   # bucket 7 = above the last


def crc16(data):
    """CRC-16/CCITT-FALSE, the same as muster_crc16()."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class FrameError(ValueError):
    pass


def decode(frame):
    """One complete frame (bytes) -> dict. Raises FrameError if it is not valid."""
    if len(frame) < 7 or frame[:2] != MAGIC or frame[2] != VERSION:
        raise FrameError("not a Muster telemetry frame")
    plen = frame[3] | frame[4] << 8
    if plen + 7 != len(frame):
        raise FrameError("length mismatch")
    if crc16(frame[2:-2]) != (frame[-2] | frame[-1] << 8):
        raise FrameError("bad CRC")
    p, o = frame[5:-2], 0

    def take(fmt):
        nonlocal o
        v = struct.unpack_from("<" + fmt, p, o)
        o += struct.calcsize("<" + fmt)
        return v

    try:
        seq, t_ms, lead, nlinks, nnodes, flags = take("HIBBBB")
        ok, failed, retries, d_last, d_avg, d_max = take("6I")
        hist = list(take("8H"))
        links = []
        for _ in range(nlinks):
            kbps, mtu, txf, txb, rxf, rxb, txfail = take("HH5I")
            links.append(dict(kbps=kbps, mtu=mtu, tx_frames=txf, tx_bytes=txb,
                              rx_frames=rxf, rx_bytes=rxb, tx_fail=txfail))
        nodes = []
        for _ in range(nnodes):
            (nid, alive, cores, nflags, inflight, mhz, ram_kb, uid, queued,
             n_ok, n_failed, n_retries, busy, last, avg, mn, mx) = take("5B2H10I")
            nodes.append(dict(id=nid, alive=bool(alive), cores=cores, flags=nflags,
                              inflight=inflight, clock_mhz=mhz, ram_kb=ram_kb, uid=uid,
                              queued=queued, ok=n_ok, failed=n_failed, retries=n_retries,
                              busy_us=busy, lat_last=last, lat_avg=avg, lat_min=mn, lat_max=mx,
                              lead=nid == lead))
        (nev,) = take("B")
        events = []
        for _ in range(nev):
            eseq, et, ev, a, b = take("HIBBB")
            events.append(dict(seq=eseq, t_ms=et, ev=ev, name=EVENTS.get(ev, str(ev)), a=a, b=b))
    except struct.error as e:
        raise FrameError(f"truncated: {e}") from None
    if o != len(p):
        raise FrameError("trailing bytes")
    return dict(seq=seq, t_ms=t_ms, lead=lead, has_us=bool(flags & 1), ok=ok, failed=failed,
                retries=retries, delay_last=d_last, delay_avg=d_avg, delay_max=d_max, hist=hist,
                links=links, nodes=nodes, events=events)


def encode(s):
    """dict (as decode() returns) -> frame bytes. Used by the demo and the tests."""
    out = bytearray(struct.pack("<HIBBBB", s["seq"] & 0xFFFF, s["t_ms"] & 0xFFFFFFFF, s["lead"],
                                len(s["links"]), len(s["nodes"]), 1 if s.get("has_us") else 0))
    m32 = 0xFFFFFFFF
    out += struct.pack("<6I", *(s[k] & m32 for k in
                                ("ok", "failed", "retries", "delay_last", "delay_avg", "delay_max")))
    out += struct.pack("<8H", *(h & 0xFFFF for h in s["hist"]))
    for lk in s["links"]:
        out += struct.pack("<HH5I", lk["kbps"], lk["mtu"], *(lk[k] & m32 for k in
                           ("tx_frames", "tx_bytes", "rx_frames", "rx_bytes", "tx_fail")))
    for n in s["nodes"]:
        out += struct.pack("<5B2H10I", n["id"], int(n["alive"]), n["cores"], n["flags"],
                           n["inflight"], n["clock_mhz"], n["ram_kb"],
                           *(n[k] & m32 for k in ("uid", "queued", "ok", "failed", "retries",
                                                  "busy_us", "lat_last", "lat_avg", "lat_min",
                                                  "lat_max")))
    out += struct.pack("<B", len(s["events"]))
    for e in s["events"]:
        out += struct.pack("<HIBBB", e["seq"] & 0xFFFF, e["t_ms"] & m32, e["ev"], e["a"], e["b"])
    body = bytes([VERSION]) + struct.pack("<H", len(out)) + bytes(out)
    return MAGIC + body + struct.pack("<H", crc16(body))


class FrameScanner:
    """Finds frames in a byte stream that may also carry ordinary text
    (Serial.print from the sketch). Feed it bytes; it yields decoded frames."""

    MAX = 4096

    def __init__(self):
        self.buf = bytearray()
        self.bad = 0

    def feed(self, data):
        self.buf += data
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                del self.buf[:-1]          # keep a trailing 'M'
                return
            del self.buf[:i]
            if len(self.buf) < 5:
                return
            plen = self.buf[3] | self.buf[4] << 8
            if self.buf[2] != VERSION or plen + 7 > self.MAX:
                del self.buf[:2]
                continue
            if len(self.buf) < plen + 7:
                return
            frame = bytes(self.buf[:plen + 7])
            try:
                yield decode(frame)
                del self.buf[:plen + 7]
            except FrameError:
                self.bad += 1
                del self.buf[:2]           # text that happened to contain "MT": resync


class DemoCluster:
    """A pretend cluster: a lead, three ESP32 workers and a Nano, on CAN and ESP-NOW.
    Every few seconds of a 60 s cycle one worker drops out and comes back.
    The numbers are SIMULATED: this shows the dashboard, not real boards."""

    def __init__(self, seed=7):
        self.rng = random.Random(seed)
        self.t_ms = 0
        self.seq = 0
        self.ev_seq = 0
        self.events = []
        self.tot = dict(ok=0, failed=0, retries=0, delay_last=0, delay_avg=0, delay_max=0)
        self.hist = [0] * 8
        self.links = [dict(kbps=500, mtu=8, tx_frames=0, tx_bytes=0, rx_frames=0, rx_bytes=0, tx_fail=0),
                      dict(kbps=1000, mtu=64, tx_frames=0, tx_bytes=0, rx_frames=0, rx_bytes=0, tx_fail=0)]
        spec = [  # id, cores, mhz, ram, uid, base latency us, share, link
            (0, 2, 240, 512, 0xA1B2C3D4, 900, 0.04, 0),
            (87, 2, 240, 320, 0x3C71A0F2, 1900, 0.34, 1),
            (142, 2, 240, 320, 0x3C71B9E8, 2100, 0.32, 1),
            (23, 2, 240, 512, 0x7E11C0DE, 1500, 0.22, 0),
            (200, 1, 16, 2, 200, 6200, 0.08, 0),
        ]
        self.nodes = []
        for nid, cores, mhz, ram, uid, base, share, lk in spec:
            self.nodes.append(dict(id=nid, alive=True, cores=cores, flags=1 if mhz > 16 else 0,
                                   inflight=0, clock_mhz=mhz, ram_kb=ram, uid=uid, queued=0,
                                   ok=0, failed=0, retries=0, busy_us=0, lat_last=0, lat_avg=0,
                                   lat_min=0, lat_max=0, _base=base, _share=share, _link=lk))
            self._event(1, nid, 0)

    def _event(self, ev, a, b):
        self.ev_seq += 1
        self.events = (self.events + [dict(seq=self.ev_seq, t_ms=self.t_ms, ev=ev, a=a, b=b)])[-8:]

    def step(self, dt_ms=250):
        self.t_ms += dt_ms
        cyc = (self.t_ms // 1000) % 60
        victim = self.nodes[2]
        if cyc == 30 and victim["alive"]:
            victim["alive"] = False
            victim["inflight"] = 0
            self._event(5, 1, victim["id"])
            self.tot["retries"] += 1
            victim["retries"] += 1
            self._event(2, victim["id"], 0)
        elif cyc == 42 and not victim["alive"]:
            victim["alive"] = True
            self._event(1, victim["id"], 0)
        wave = 1.0 + 0.45 * math.sin(self.t_ms / 9000.0)
        calls = max(1, int(dt_ms / 1000 * 60 * wave + self.rng.random() * 3))
        live = [n for n in self.nodes if n["alive"]]
        weights = [n["_share"] for n in live]
        for _ in range(calls):
            n = self.rng.choices(live, weights)[0]
            lat = int(n["_base"] * (0.8 + 0.5 * self.rng.random()) * (1.6 if wave > 1.3 else 1.0))
            ok = self.rng.random() > 0.003
            n["ok" if ok else "failed"] += 1
            n["busy_us"] += lat
            n["lat_last"] = lat
            n["lat_min"] = lat if n["ok"] + n["failed"] == 1 else min(n["lat_min"], lat)
            n["lat_max"] = max(n["lat_max"], lat)
            n["lat_avg"] = lat if n["ok"] + n["failed"] == 1 else n["lat_avg"] + (lat - n["lat_avg"]) // 8
            b = sum(lat >= e for e in HIST_EDGES_US)
            self.hist[b] += 1
            d = lat + self.rng.randint(80, 400)
            self.tot["ok" if ok else "failed"] += 1
            self.tot["delay_last"] = d
            self.tot["delay_max"] = max(self.tot["delay_max"], d)
            self.tot["delay_avg"] = d if self.tot["ok"] == 1 else self.tot["delay_avg"] + (d - self.tot["delay_avg"]) // 8
            lk = self.links[n["_link"]] if n["id"] != 0 else None
            if lk:
                frames = 2 if lk["mtu"] > 8 else 6
                lk["tx_frames"] += frames // 2
                lk["rx_frames"] += frames // 2
                lk["tx_bytes"] += 4 * frames
                lk["rx_bytes"] += 20 * frames
        for n in self.nodes:
            n["inflight"] = self.rng.randint(0, 2) if n["alive"] and n["id"] != 0 else 0
            n["queued"] = n["inflight"] * 10
        for lk in self.links:        # heartbeats
            lk["tx_frames"] += dt_ms // 200
            lk["tx_bytes"] += dt_ms // 200 * 8
            lk["rx_frames"] += dt_ms // 200 * 3
            lk["rx_bytes"] += dt_ms // 200 * 24
        if self.rng.random() < 0.02:
            self.links[1]["tx_fail"] += 1
        if self.rng.random() < 0.3:
            self._event(4, 1, self.rng.choice(live)["id"])
        self.seq += 1
        nodes = [{k: v for k, v in n.items() if not k.startswith("_")} for n in self.nodes]
        return encode(dict(seq=self.seq, t_ms=self.t_ms, lead=0, has_us=True, hist=self.hist,
                           links=self.links, nodes=nodes, events=self.events, **self.tot))

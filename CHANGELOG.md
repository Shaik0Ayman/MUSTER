# Changelog

All notable changes are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project is pre-1.0:
the wire protocol version (`MUSTER_PROTO_VERSION`) is the compatibility contract,
and boards on different protocol versions refuse each other at `JOIN`.

## [Unreleased]

### Added
* **Telemetry and the dashboard (roadmap M8).** Optional lead-side counters
  (`MUSTER_TELEMETRY`, off by default, compiled out when off): per-board calls, failures,
  retries, latency (last/avg/min/max) and busy time; per-link frames, bytes and send
  failures; end-to-end call delay; a latency histogram; the last events.
  `muster_telemetry_frame()` writes a CRC-protected binary snapshot (docs/TELEMETRY.md);
  `muster_set_micros()` gives µs latencies. No wire-protocol change.
* `muster dash`: the dashboard hosted on your PC, reading the lead's USB serial
  (`--serial`, needs pyserial), a capture (`--replay`) or a simulated cluster (`--demo`).
* `muster_dashboard.h`: optional on-board hosting of the same page for an ESP32 lead.
  `firmware/examples/dashboard` shows both (compiles for ESP32; not run on hardware).
* `firmware/examples/portability` and `docs/BOARDS.md`: one self-test sketch built for
  boards across MCU families (AVR, megaAVR, ESP32/ESP8266, STM32, RP2040, nRF52, SAM/SAMD,
  Teensy (NXP), Renesas RA); flashed, it runs a whole cluster inside the chip and prints
  PASS/FAIL. All 22 boards compile; none have been flashed with it yet.
* Loopback link sizes (`LB_MAX_NODES`, `LB_QUEUE`, `LB_FRAME`) can be overridden.
* README rewritten for newcomers, with illustrations (docs/images/).
* **Lead-assigned modules** (`MUSTER_MODULE`): long-running units the lead hands to
  boards, re-places when a board is lost, and moves away from a board that declines
  to start them. Wire message `ASSIGN`, module masks in `JOIN`/`BEAT`.
* **Shared images** (`shared_images: true`): one firmware build per board *type*,
  with node IDs derived from the chip's factory MAC and ID clashes resolved by the
  lead (`ACK ok=4`).
* Modules and tasks may live in project-local headers next to the sketch; a module
  may be declared early and defined after the generated glue so it can call tasks.
* Optional `muster_link.set_id` so a CAN link learns an ID that changed after start.
* `muster_link_uart.h`: a point-to-point link over any byte stream (sync, length, CRC-16;
  resyncs after corruption) for testing Muster between boards without CAN, for example in
  simulators. Host-tested; worked on real boards (2026-11-07, no latency measured).
* `firmware/examples/sim_cluster`: a lead and two workers in one simulated ESP32-S3
  (Wokwi project, LEDs and buttons), with its logic also run as a host test.
* `muster_link_espnow.h`: Muster over ESP-NOW for ESP32 boards with no CAN transceiver
  (wireless broadcast, src ID in the packet, lock-free receive ring). The ring and packet
  logic is host-tested (`test_espnow_link`). **Run on hardware** (2026-11-07) in the `trio` demo:
  join, calls, failover and rejoin worked; radio latency not yet measured.
* `firmware/examples/trio`: three-ESP32 demo (ESP32-C6 lead + two DevKit workers). A Fourier-series
  task runs on the workers, each answer sets a PWM duty on its board, and the lead serves a live
  per-board dashboard from its own WiFi access point. Includes a per-board self-test sketch and a
  hardware-free dashboard preview. Task math host-tested (`test_trio_task`). **Run on hardware** (2026-11-07,
  ESP32-C6 + 2 DevKits): average task time ≈ 900 µs per call on a worker (compute only).
* GPL-3.0-only license, README, CONTRIBUTING, SECURITY, issue and PR templates.

### Fixed
* `muster plan` crashed with `UnicodeEncodeError` on a Windows console when a plan was
  rejected (the console's cp1252 can't print the error marks), hiding why it was
  rejected. A missing or malformed `--plan` file now gives a clean error, not a traceback.
* `trio_mock.py` failed to start on Windows (read `dashboard.h` with the system encoding).
* `muster_link_espnow.h`: the radio code was compiled on *any* Arduino build (or any
  ESP-IDF build) and failed with `WiFi.h: No such file or directory` on an Arduino
  Nano or a plain ESP-IDF project. It is now limited to Arduino-ESP32
  (`ARDUINO_ARCH_ESP32`).
* `trio` lead: `/api/state` built its JSON with unchecked `snprintf` offsets. Once the
  4 KB buffer filled, the remaining size wrapped round and the next write ran past the
  end. Appends are now clamped.
* `trio` lead: the calls-per-second meter only updated on passes that were due to
  issue a call; it now updates on every pass.
* `trio_task.h`: the result-layout comment said 31 bytes; it is 29
  (`TRIO_RESULT_BYTES`, now checked by `test_trio_task`).

### Added (tooling and docs)
* Compile-time limits on `MUSTER_UART_MTU` (≤ 253), `MUSTER_ESPNOW_MTU` (≤ 249) and
  `MUSTER_ESPNOW_RING` (2–255), which the links' `uint8_t` lengths and indices need.
* `make -C firmware/test` now also runs `guards` (link headers compile with no
  platform, as on an Arduino AVR build) and `check-copies` (the `trio` sketch's
  copies of the headers match `firmware/src/`); `sync-copies` refreshes them.
* CI builds the `trio` and `trio/node_selftest` examples.
* `docs/LINKS.md` (every transport, how to attach it, its limits and status),
  `firmware/examples/README.md`, ESP-NOW and UART notes in `docs/HARDWARE.md`,
  `CODE_OF_CONDUCT.md`.

### Changed
* **Protocol v3** (incompatible with v2): `JOIN` 14 → 19 bytes, `ACK` gains a `uid`,
  `BEAT` gains `running`/`refused`. See `docs/PROTOCOL.md`.
* Minimal ATmega328P worker: 8120 → 8854 B flash, 468 → 492 B static RAM.
* Host tests: 101 → 215 checks in 13 binaries. Toolchain tests: 63 → 83.

### Not yet verified
* The CAN and SPI drivers and everything built on them have **not run on real
  hardware** (roadmap M4, M5): see `docs/ROADMAP.md`.

## Earlier work (milestones M0–M9)
Protocol v2 runtime with multi-link routing, duplicate suppression and failover;
the CAN (TWAI, MCP2515) and SPI links; the libclang analyser and typed trampoline
generator; the MCP server replacing the local-model partitioner. Details and
acceptance evidence: `docs/ROADMAP.md`, `docs/DECISIONS.md`.

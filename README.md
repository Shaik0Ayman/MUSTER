# Muster

**A framework for multi-MCU embedded systems.** Separate boards, different
families, different buses — one scheduler deciding who does what.

- `firmware/` — `muster.h`, a single-header C99 runtime, plus CAN (ESP32 TWAI,
  MCP2515) and SPI transports, host tests and PlatformIO examples.
- `tools/muster-cli/` — splits one annotated program across your boards (local
  Qwen3.5-9B proposes, a validator verifies), generates per-board PlatformIO
  builds and a wiring diagram.
- `docs/` — protocol, architecture, specs for unbuilt parts, hardware guide,
  decision log, roadmap with acceptance criteria.

## Status
Runtime verified on simulated buses: 101 host checks in C99 and C++11, covering
failover, duplicate suppression, 5% frame loss, CAN + SPI multi-link routing and
SPI bit-flip recovery. Minimal ATmega328P worker: 8.1 KB flash, 468 B static RAM.
CAN and ESP32-S3 SPI drivers compile for their targets, **not yet verified on
hardware**. See `docs/ROADMAP.md`.

## Quick start
```bash
make -C firmware/test                       # host tests
pio run -d firmware/examples/lead_esp32     # build an example
pip install -e tools/muster-cli
muster plan tools/muster-cli/examples/quadruped/boards.yaml \
            tools/muster-cli/examples/quadruped/main.cpp --offline
```



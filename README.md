# Muster

**A framework for multi-MCU embedded systems.** Separate boards, different
families, different buses — one scheduler deciding who does what.

- `firmware/` — `muster.h`, a single-header C99 runtime, plus CAN (ESP32 TWAI,
  MCP2515) and SPI transports, host tests and PlatformIO examples.
- `tools/muster-cli/` — splits one annotated program across your boards (the
  baseline or an assistant over MCP proposes, a validator verifies), generates
  typed per-board PlatformIO builds and a wiring diagram. `muster mcp` exposes it
  to Claude Code or any MCP client.
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
            tools/muster-cli/examples/quadruped/main.cpp
muster build                                # compile it for every board
```

## Building with Claude Code
`CLAUDE.md` (root) plus the scoped `firmware/CLAUDE.md` and
`tools/muster-cli/CLAUDE.md` give Claude Code the project's rules and context
automatically. Start with `docs/CLAUDE_CODE_PROMPTS.md`.
Docs: https://docs.claude.com/en/docs/claude-code/overview

Team: Shaik Ayman Hameed Baig, Lahari — SRMIST, ECE.

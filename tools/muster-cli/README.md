# muster-cli

The toolchain for [Muster](https://github.com/Shaik0Ayman/MUSTER): split one annotated
Arduino-style program across several microcontroller boards.

```text
main.cpp + boards.yaml → analyse (libclang) → partition → validate → generate → pio run → deploy
```

## Install

```bash
pip install -e ".[test]"      # from tools/muster-cli in a clone of the repository
```

Python 3.9+. Dependencies: `pyyaml`, `libclang`, `wireviz`; `pyserial` only for
`muster dash --serial` (`pip install -e ".[dash]"`). To compile for boards you
also need [PlatformIO](https://platformio.org/); Graphviz is optional and renders the
wiring diagram.

## Use

```bash
muster boards                                       # board types it knows
muster analyze examples/quadruped_modules/main.cpp  # tasks, modules, wire types, problems
muster plan examples/quadruped_modules/boards.yaml examples/quadruped_modules/main.cpp
muster build                                        # compile every board (pio run)
muster deploy examples/quadruped_modules/boards.yaml --dry-run   # upload, lead last
```

| Command | Does |
|---|---|
| `muster boards` | list the board types in the database |
| `muster analyze main.cpp [--json]` | tasks, modules, wire types and problems |
| `muster plan boards.yaml main.cpp [--plan plan.json] [--out DIR]` | validate a plan (default: the baseline) and generate the project |
| `muster build [-e ENV ...]` | compile every board's firmware |
| `muster deploy boards.yaml [--dry-run]` | upload every board, lead last, stop at the first failure |
| `muster dash --serial PORT \| --demo \| --replay FILE` | live cluster dashboard, hosted on this PC ([docs](https://github.com/Shaik0Ayman/MUSTER/blob/main/docs/TELEMETRY.md)) |
| `muster mcp` | serve the same operations to an AI assistant over MCP (stdio) |

Examples: [`examples/quadruped`](examples/quadruped) (one image per board) and
[`examples/quadruped_modules`](examples/quadruped_modules) (shared images plus
lead-assigned modules).

## What it guarantees

* **The validator is the final authority.** Every plan, whether it comes from the
  baseline, a file or an assistant over MCP, passes `validate()` before code is
  generated, and generation re-validates on its own.
* **Pins come only from the board database** ([`muster_cli/db/boards.yaml`](muster_cli/db/boards.yaml)).
  Plans never contain pins. To add a board, add a datasheet-backed entry there.
* **Wire types are fixed-width.** `int`, `long`, `double`, pointers and similar are
  rejected at plan time, with a message naming the replacement.

## Limits

* Board types: `esp32-s3-devkitc-1` and `nanoatmega328`.
* Generated projects talk over **CAN** (TWAI on the ESP32-S3, MCP2515 on the Nano).
  The runtime also has SPI, UART and ESP-NOW links; to use one in a generated project,
  build with `MUSTER_GEN_LINK_EXTERNAL` and define
  `muster_link *muster_gen_external_link(void)` in your sketch
  ([link guide](https://github.com/Shaik0Ayman/MUSTER/blob/main/docs/LINKS.md)).
* The generated CAN projects compile for every board but have **not been run on
  hardware yet**.

## Tests

```bash
pytest                       # 83 tests: analyser, validator, codegen + host round trip, MCP, CLI
MUSTER_PIO_TEST=1 pytest     # also compile generated projects for real boards
```

On Windows, run Python natively and set `MUSTER_HOST_CXX=wsl` so the host round trip
compiles inside WSL.

## Use from an AI assistant

```bash
claude mcp add muster -- muster mcp
```

The assistant proposes the partition; the validator decides. `deploy` over MCP is a
dry run unless called with `confirm=true`.

Full documentation is in the [main README](https://github.com/Shaik0Ayman/MUSTER#readme).
Licensed under GPL-3.0-only.

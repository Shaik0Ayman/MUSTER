# muster-cli

Split one embedded program across many microcontroller boards.

You write one `main.cpp`, mark the functions that may run elsewhere, list the
boards you own, and muster produces a PlatformIO project with one environment
per board, builds and uploads it, and draws the wiring. Deciding *which boards
hold which tasks* is either the deterministic baseline or an assistant (Claude
Code, or any MCP client) using muster's tools; either way the validator has the
final word.

## Setup

```bash
python -m venv .venv && . .venv/bin/activate     # Windows: .venv\Scripts\activate
pip install -e ".[test]"                         # pyyaml, libclang, wireviz (+ pytest)
```

PlatformIO (`pip install platformio`, or the VS Code extension) builds and
uploads. Graphviz (`apt install graphviz` / `brew install graphviz` / the
Windows installer) is only needed to render the wiring picture; without it
muster writes `wiring.yaml` and the notes and tells you how to render later.

## Use from the command line

```bash
muster analyze examples/quadruped/main.cpp           # tasks, wire types, problems
muster plan    examples/quadruped/boards.yaml examples/quadruped/main.cpp
muster build                                         # compile every board
muster deploy  examples/quadruped/boards.yaml        # upload, lead last (--dry-run to preview)
```

`muster plan` uses the deterministic baseline; `--plan my_plan.json` uses your
own plan instead (same shape as `muster_out/plan.json`).

## Use from an assistant (MCP)

`muster mcp` serves the toolchain over stdio. The repo's `.mcp.json` registers
it for Claude Code; for another client, run `muster mcp` (or
`python -m muster_cli.mcp_server`) as a stdio server.

| Tool | What |
|---|---|
| `list_boards` | board types, capabilities, RAM, CAN hardware, max call sizes |
| `analyze_source` | tasks, wire types, encoded sizes, errors to fix in the source |
| `baseline_plan` | the deterministic plan, as a starting point |
| `validate_plan` | errors (binding) and warnings for any plan |
| `generate_project` | re-validates, then writes the project; refuses invalid plans |
| `render_wiring` | wiring.yaml / SVG / notes from the board DB |
| `build_project` | `pio run` for every board (compile only) |
| `deploy` | upload, lead last; **dry run unless `confirm: true`** |

Then ask the assistant something like *"split examples/quadruped/main.cpp
across the boards in examples/quadruped/boards.yaml and build it"*. The server
sends it the planning rules when it connects.

## Annotating code

```c
MUSTER_TASK(needs = fpu, cost = 300, ram = 8)
Vec3 imu_filter(Vec3 accel, Vec3 gyro) { ... }
```

`needs` takes capability flags joined by `|` (`fpu`, `wifi`, `ble`); `cost` is
relative work (1..65535, used for placement); `ram` is KB.

A task may cross boards only if its parameters and result are values of:
`uint8/16/32_t`, `int8/16/32_t`, `float`, `bool`, and structs of those
(nested, with fixed-size arrays). The analyser rejects `int`, `long`,
`unsigned`, `short`, `char`, `double`, `size_t`, enums, unions, bit-fields,
pointers, references and array parameters, naming the fixed-width replacement:
those types differ between 8-bit AVR and 32-bit ESP32. A task must not touch
mutable globals (`const` ones are fine).

## What the generated code gives you

`muster plan` copies your sketch and adds `#include "muster_gen.h"` after the
last task. In your `setup()`/`loop()`:

```c
void setup() { muster_setup(); }          // CAN link + runtime + resident tasks
void loop() {
    muster_loop();                        // = muster_poll()
    Vec3 att;
    if (muster_imu_filter(accel, gyro, &att, 300) == MUSTER_OK) { ... }  // runs wherever the lead puts it
}
```

Each task `T(args)` gets `muster_T(args, &out, timeout_ms)` (blocking; never
call it from inside a task), and the pair `muster_T_async(args)` /
`muster_T_result(handle, &out)`. `MUSTER_LEAD` is 1 on the lead's build, so
one sketch can give the lead the control loop.

## Pipeline

1. **analyze** (`analyze.py`, libclang) - tasks, wire types, encoded sizes,
   globals touched. Facts only. Parses without any board SDK: stand-in
   headers live in `muster_cli/shim/`, unknown headers become empty stubs.
2. **partition** - the baseline (`partition.py`), a plan file, or an assistant
   over MCP.
3. **validate** (`validate.py`) - final authority: non-portable types,
   globals, capabilities, RAM, message size on the smallest board, node and
   task limits.
4. **generate** (`codegen.py`, `pio.py`) - `muster_gen.h` (struct encoders,
   trampolines, setup, call stubs), `muster_plan.h` (task IDs),
   `platformio.ini` (one env per board), runtime headers copied from
   `firmware/src`, wiring (`wiring.py`).

Pins come only from `muster_cli/db/boards.yaml`. Nothing that proposes a plan
ever chooses a pin.

## Outputs (`muster_out/`)

| File | What |
|---|---|
| `plan.json` | the plan, who submitted it, validation results |
| `platformio.ini` | one env per board: node ID, lead flag, resident mask, CAN pins, port |
| `src/main.cpp` | your sketch + the two generated includes |
| `src/muster_runtime.cpp` | the runtime's single `MUSTER_IMPLEMENTATION` |
| `include/muster_gen.h` | typed glue (see above) |
| `include/muster_plan.h` | task IDs |
| `include/muster*.h` | runtime + link headers, copied from `firmware/src` |
| `wiring.yaml` / `wiring.svg` / `wiring.png` | connection diagram |
| `WIRING_NOTES.txt` | termination, grounding, crystal, level-shifting warnings |

## Tests

```bash
pytest                                   # analyser, validator, codegen, MCP, CLI
MUSTER_PIO_TEST=1 pytest                 # also compile the example for every board
```

The host round-trip test compiles the generated glue with g++ and calls each
task through its stub; on Windows without g++, `MUSTER_HOST_CXX=wsl` runs it in WSL.

## Known limits

| Area | Now | Next |
|---|---|---|
| Struct names | must be defined in the sketch, named, plain (no bases/virtuals) | headers from libraries |
| Links | one CAN link per board from the DB (or `-DMUSTER_GEN_LINK_EXTERNAL`) | SPI accelerator generated from the DB |
| MCP2515 crystal | 8 MHz assumed | from the inventory |
| Board DB | ESP32-S3, Nano, MCP2515, SN65HVD230 | STM32, Teensy 4.x, RP2040 |

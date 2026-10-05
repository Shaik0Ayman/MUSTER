# muster-cli

Split one embedded program across many microcontroller boards.

You write one `main.cpp`, mark the functions that may run elsewhere, list the
boards you own, and muster produces a PlatformIO project with one environment
per board, uploads them, and draws the wiring.

## Setup

```bash
pip install -e .
ollama pull qwen3.5:9b          # Q4_K_M by default, ~6.6 GB, fits an 8 GB GPU
```

Graphviz must be installed for diagrams (`apt install graphviz` / `brew install graphviz`).

## Use

```bash
muster plan   examples/quadruped/boards.yaml examples/quadruped/main.cpp
muster deploy examples/quadruped/boards.yaml          # add --dry-run to preview
```

`--offline` skips the LLM and uses the deterministic baseline partitioner.

## Annotating code

```c
MUSTER_TASK(needs = fpu, cost = 300, ram = 8)
Vec3 imu_filter(Vec3 accel, Vec3 gyro) { ... }
```

`needs` takes capability flags joined by `|` (`fpu`, `wifi`, `ble`).
A task may cross boards only if it:
- takes and returns values, never pointers or references
- touches no global variables

The validator refuses anything else, with a message saying why.

## Pipeline

1. **analyze** - extracts tasks, needs, pointer params, globals touched. Facts only.
2. **partition** - Qwen3.5-9B via Ollama with JSON-schema constrained output,
   or the deterministic baseline. Decides which boards hold which task images.
3. **validate** - final authority. Rejects unsafe source, missing capabilities,
   RAM overflow. LLM plans that fail are sent back with the errors, up to 3 repairs.
4. **generate** - `platformio.ini`, task-ID header, residency registry,
   WireViz wiring diagram (SVG + PNG) and wiring notes.

Pins come only from `muster_cli/db/boards.yaml`. The LLM never chooses a pin.

## Outputs (`muster_out/`)

| File | What |
|---|---|
| `plan.json` | the partition, its rationale, validation results, attempt count |
| `platformio.ini` | one env per board: node ID, lead flag, residency mask, upload port |
| `include/muster_plan.h` | generated task IDs |
| `src/muster_registry_gen.cpp` | registers only resident tasks; `--gc-sections` drops the rest |
| `wiring.svg` / `wiring.png` | connection diagram |
| `WIRING_NOTES.txt` | termination, grounding, level-shifting warnings |

## Prototype vs production

| Area | Now | Next |
|---|---|---|
| Analyser | brace-aware regex scanner | libclang (macros, includes, aliasing) |
| Task registry | casts to `void(*)(void)` | typed stubs with argument serialisation |
| Runtime | not included | `muster` C99 runtime (core / port / link) |
| Board DB | ESP32-S3, Nano, MCP2515, SN65HVD230 | STM32, Teensy 4.x, RP2040 |

## Evaluating the LLM

Run every test program twice - once with the model, once `--offline` - and compare
from `plan.json`: validation-pass rate on first attempt, repair attempts, and how
often the LLM plan beats the baseline (fewer single-board tasks, better lead choice).
If it cannot beat the baseline, the baseline ships.

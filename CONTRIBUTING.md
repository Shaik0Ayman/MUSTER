# Contributing to Muster

Thank you for helping. Muster runs on boards with 2 KB of RAM, so a few rules are
strict; they exist because each one protects a real failure mode. Read this once,
then the spec for the area you are touching.

## Good first contributions
* **Hardware results.** The CAN and SPI links have only run in simulation; UART and
  ESP-NOW have run on boards but have no latency measurements yet.
  Follow the bring-up order in [`docs/HARDWARE.md`](docs/HARDWARE.md) and open a
  *Hardware report* issue, whether it worked or not.
* **A new board** in `tools/muster-cli/muster_cli/db/boards.yaml`, backed by its
  datasheet (cite it in the PR). Never guess a pinout.
* **A failing host test** for a bug you found. That is the best bug report.

## Setup
```bash
git clone https://github.com/Shaik0Ayman/MUSTER.git && cd MUSTER
pip install -e "tools/muster-cli[test]"      # toolchain + pytest
pip install platformio                       # to compile for boards
```
`make` and a C/C++ compiler run the firmware tests. On Windows use WSL for those and
set `MUSTER_HOST_CXX=wsl` so the toolchain tests compile their host round trip there.

## Checks every change must pass
```bash
make -C firmware/test            # strict C99: -Wall -Wextra -Wpedantic -Werror (also link guards + sketch copies)
make -C firmware/test cpp        # the same tests as C++11 (Arduino compiles as C++)
make -C firmware/test avr        # after core changes: ATmega328P flash/RAM, needs avr-gcc
pytest tools/muster-cli          # toolchain tests
MUSTER_PIO_TEST=1 pytest tools/muster-cli   # after generator changes: compile for real boards
```
Report the AVR footprint delta in your PR. Worker static RAM must stay **under 600 B**.

## Hard rules
1. **No dynamic allocation, no recursion, no VLAs** in `firmware/`. Everything is
   static and sized by `MUSTER_*` macros. It must build for the ATmega328P.
2. **C99 core, C++11-compatible.** `muster.h` compiles clean under
   `-std=c99 -Wall -Wextra -Wpedantic -Werror` and as C++11.
3. **Never send raw structs or native `int`/`double` on the wire.** `int` is 16-bit on
   AVR and 32-bit on ESP32. Use `muster_put_*` / `muster_get_*` (fixed width,
   little-endian).
4. **Wire protocol changes bump `MUSTER_PROTO_VERSION`** and update
   `docs/PROTOCOL.md` in the same change.
5. **Every behaviour change to the core gets a host test** in `firmware/test/`
   (`test_<area>.c`, added to `TESTS` in `firmware/test/Makefile`).
6. **Single-threaded model.** `muster_poll()` is called from one context. No locks or
   threads in the core.
7. **Pins come only from `boards.yaml`.** Plans never contain pins, and nothing that
   proposes a plan chooses them. A hallucinated pin is a burnt board.
8. **The validator is the final authority.** No plan (baseline, file, or from an
   assistant over MCP) reaches code generation without `validate()`;
   `pipeline.make()` re-validates itself. Do not add a path around it.
9. **Do not claim hardware behaviour you have not observed.** Mark anything untested on
   real boards as such, in code comments and docs.
10. **Keep the RAM budget.** Check `make -C firmware/test avr` after core changes.

Intentional limits (at-least-once across boards, hub and spoke, tasks must not call
`muster_call()`, and so on) are listed in the README and explained in
[`docs/DECISIONS.md`](docs/DECISIONS.md). Please do not "fix" them without reading why.

## Style
* **C:** 4-space indent, `snake_case`, public API prefixed `muster_`, internal `m_`
  (and `static`). Comment *why*, not what. Keep functions short; the core is meant to
  be readable. Platform code (ESP-IDF, Arduino, MCP2515) lives only in
  `muster_link_*.h`, guarded so host tests never see it.
* **Python:** 3.9+, no new heavy dependencies without discussion. The analyser's output
  dict is a contract: add keys, do not rename them. Wire types live in `wiretypes.py`;
  a new type needs its codec in `muster.h` first.
* **Generated code** uses the `mg_` / `muster_` prefixes and the fixed-width rules above.
* **Commits:** `area: what changed`, for example `core: dedupe repeated CALLs`.
* **New source files** start with the SPDX header used in the existing ones.
* **The `trio` sketch** carries copies of `firmware/src/*.h` (the Arduino IDE only builds
  files in the sketch folder). After changing a header, run `make -C firmware/test sync-copies`.

## Specs to read first
| Area | Read |
|---|---|
| Core runtime and message flow | `docs/ARCHITECTURE.md`, `docs/PROTOCOL.md` |
| CAN + SPI together | `docs/MULTILINK_SPEC.md` |
| SPI transport | `docs/SPI_LINK_SPEC.md` |
| Any transport (UART, ESP-NOW, writing a new link) | `docs/LINKS.md` |
| Wiring, pins, electrical | `docs/HARDWARE.md` |
| Why things are as they are | `docs/DECISIONS.md` (add new decisions at the bottom) |
| Testing and benchmarks | `docs/TESTING.md` |

## Conduct
This project follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## License of contributions
Muster is licensed under **GPL-3.0-only** (see [`LICENSE`](LICENSE)). By opening a pull
request you agree that your contribution is licensed under the same terms.

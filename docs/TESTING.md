# Testing

## Host tests (every change)
`make -C firmware/test` and `make -C firmware/test cpp`. The loopback link
(`muster_link_loopback.h`) is an in-memory broadcast bus that behaves like CAN
where it matters: every frame reaches every node, frames are capped at the MTU,
and a port can be "unplugged".

| Binary | Covers |
|---|---|
| `test_bus` | the original three-board scenario (join, fragmentation, placement, failover, rejoin) |
| `test_telemetry` | the lead's optional counters and snapshot frame: CRC, sequence, per-board latency and load with a µs clock, a board going silent, the event window |
| `test_core` | duplicate suppression, worker resends, slow tasks, liveness/rejoin, lead restart, hostile input, API edges |
| `test_framing` | reassembly edge cases, link MTU checks, node table full/recycle, version check |
| `test_loss` | 5% random frame loss, 300 calls (with `MUSTER_MAX_RETRIES=6`) |
| `test_modules` | lead-assigned modules (placement, decline, loss, healed ASSIGN) and uid-based IDs with clash resolution |
| `test_sim_cluster` | the Wokwi demo (`examples/sim_cluster`): pull worker cables, module moves, rejoin |
| `test_espnow_link` | the ESP-NOW link's ring and packet logic, then a lead with two workers on a simulated lossy broadcast medium (10% loss, node-ID change, a worker leaving) |
| `test_trio_task` | the trio demo's task: the series matches its closed form, result encoding round-trips, the PWM hook gets the reported duty |
| `test_uart_link` | the byte-stream link: framing, resync after corruption, a lead with two workers on separate cables, a pulled cable |
| `test_multilink` | CAN + SPI-like buses: routes, link-priced placement, unplug/replug, broadcasts |
| `test_spi_frame` | SPI packet framing and CRC |
| `test_spi_link` | SPI queues, master/slave logic, two boards over the in-memory wire, bit flips, cable pull |

`make -C firmware/test` also runs two build checks before the binaries:

* `guards`: every portable link header compiles as plain C with `ARDUINO` defined and
  no ESP32, the way an Arduino AVR build sees it, so platform code can't leak into a
  board that doesn't drive it.
* `check-copies`: the headers inside `examples/trio/muster_trio/` are identical to
  `src/` (`make sync-copies` refreshes them).
* `check-dashboard`: `src/muster_dashboard_page.h` matches the dashboard page it is
  generated from (`tools/muster-cli/muster_cli/dash/embed.py`; needs `python3`).

The toolchain's `tests/test_telemetry.py` round-trips frames, finds frames in a serial
stream mixed with text, serves the dashboard over HTTP, and decodes a frame produced by
the real C encoder (`test_telemetry.c` built on the host).

**Portability:** `firmware/examples/portability` builds one self-test sketch for boards
across MCU families (results in [`BOARDS.md`](BOARDS.md)). Flashed, it runs a lead and
workers inside one chip and prints `MUSTER SELFTEST: PASS/FAIL`.

## Hardware results

Reported by the maintainers. Each row says what was observed and, just as
important, what wasn't measured.

| Date | Link | Boards | Observed | Not measured / not recorded |
|---|---|---|---|---|
| 2026-11-07 | ESP-NOW | ESP32-C6 (lead, `trio`) + 2 × ESP32 DevKit (workers) | workers joined; calls were placed on both workers; dashboard served by the C6 over its own access point while ESP-NOW ran; a worker switched off was declared lost and the other took the calls; it rejoined when powered back. Average task time per call on a worker: **≈ 900 µs** (dashboard `avg`, the worker's `busy_us` for `wave(step, 256)`). | radio round-trip latency (call → result), failover time, packet loss rate, PWM output, `node_selftest`, how the C6 image was built |
| 2026-11-07 | UART | not recorded | the link worked between real boards | boards, baud rate, latency |

**What ≈ 900 µs is.** `busy_us` is measured on the worker around the computation only
(`trio_wave_tr`), so it is the cost of 256 `sinf` terms on a 240 MHz ESP32. It does
not include the radio. A call's end-to-end latency is that plus two ESP-NOW transfers
plus queueing on both sides. To measure it, time `muster_call()` on the lead with
`micros()`, or toggle a GPIO at call and result and read it with a logic analyser,
then add a row here.

`rig.h` holds the shared rig: fake clock, `CHECK`, `rig_step()` (polls every
board once per ms) and `rig_wait()`. A task that should "take time" calls
`rig_step()` itself: the other boards keep running while its own board is busy.
The loopback bus can unplug or mute a port, drop deliveries at random, or drop
frames chosen by a hook.

Rules:
- Tests that simulate several boards in one process must use the **async API**
  (`muster_call_async` + `muster_result`) and a step function that polls every
  node. `muster_call()` only polls its own node.
- Use the fake clock (`g_now`), never real time.
- Each test prints PASS/FAIL lines and the binary exits non-zero on failure.

## Footprint (after core changes)
`make -C firmware/test avr` — report Program, Data, and the largest stack frames.
Budget: worker static RAM < 600 B; total worker RAM incl. stack < 1 KB.

## Hardware benchmarks (M4, M5)
| Metric | How |
|---|---|
| Per-hop latency | logic analyser on CANH or SPI SCK; CALL first frame → RESULT last frame |
| Remote vs local | same task run locally and remotely, N = 1000, report median and p99 |
| Bus utilisation | frames/s × frame bits ÷ bit rate; cross-check on analyser |
| Failover time | power cut on worker → RESULT delivered from another board |
| Control-loop jitter | toggle a GPIO in the lead's control loop; histogram with traffic on vs off |
| Footprint | `avr-size`, ESP32 build size report |

Baselines: single-board firmware doing the same work, and a naive UART master/slave link.

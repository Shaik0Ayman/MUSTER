# Testing

## Host tests (every change)
`make -C firmware/test` and `make -C firmware/test cpp`. The loopback link
(`muster_link_loopback.h`) is an in-memory broadcast bus that behaves like CAN
where it matters: every frame reaches every node, frames are capped at the MTU,
and a port can be "unplugged".

| Binary | Covers |
|---|---|
| `test_bus` | the original three-board scenario (join, fragmentation, placement, failover, rejoin) |
| `test_core` | duplicate suppression, worker resends, slow tasks, liveness/rejoin, lead restart, hostile input, API edges |
| `test_framing` | reassembly edge cases, link MTU checks, node table full/recycle, version check |
| `test_loss` | 5% random frame loss, 300 calls (with `MUSTER_MAX_RETRIES=6`) |
| `test_modules` | lead-assigned modules (placement, decline, loss, healed ASSIGN) and uid-based IDs with clash resolution |
| `test_multilink` | CAN + SPI-like buses: routes, link-priced placement, unplug/replug, broadcasts |
| `test_spi_frame` | SPI packet framing and CRC |
| `test_spi_link` | SPI queues, master/slave logic, two boards over the in-memory wire, bit flips, cable pull |

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

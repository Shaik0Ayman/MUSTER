# Roadmap

Work milestones in order. Each has acceptance criteria; a milestone is done only
when every box can be ticked with evidence (test output, footprint numbers,
analyser capture). Update the checkboxes as you go.

**Course demo path:** M0 → M1 → M4 → M6. SPI (M2, M3, M5) can follow.

## M0 — Repo bootstrap
- [x] `make -C firmware/test` and `make -C firmware/test cpp` pass locally.
      *(2026-10-05, gcc/g++ 13.3 under WSL Ubuntu 24.04: 7 test binaries,
      101 checks, C99 and C++11.)*
- [ ] CI (`.github/workflows/ci.yml`) runs host tests, builds all three examples, runs `muster plan --offline`.
      *(Workflow written; it has not run yet — it runs on the first push.)*
- [x] All three examples build with `pio run`.
      *(2026-10-05, PlatformIO with Arduino-ESP32 2.0.17 / Arduino AVR: `worker_nano`,
      `lead_esp32`, `lead_esp32_spi`, `worker_esp32`, `worker_esp32_spi` all SUCCESS.)*

## M1 — Core hardening
- [x] `MUSTER_MAX_FRAME` replaces the hard-coded 64-byte frame buffers.
- [x] **Duplicate suppression:** workers keep a small per-origin cache of recent
      `(origin, call_id)` and re-send the cached status instead of re-running a task.
      Size it for AVR. Test: a slow worker plus a retry runs the task exactly once.
      *(Cache key adds task + CRC-16 of args; entries hold the full result.
      `MUSTER_DEDUPE` = 1 on AVR, 4 elsewhere. The lead keeps the same cache for
      relayed results, and workers now resend their own calls. Exactly-once holds
      for retries to the same board; a call moved to another board can still run
      twice — see PROTOCOL.md. Tests: `test_core.c` sections 1–3.)*
- [x] Framing edge-case tests: fragment gap, reassembly overflow, stale slot expiry,
      interleaved fragments from two sources, frame for another node dropped before buffering.
      *(`test_framing.c`, plus two-link interleaving, restart, full slots, full node table.)*
- [x] Loss test: random 5% frame drop on the loopback bus; all calls still complete.
      *(`test_loss.c`: 300/300 calls correct, 602 of 11846 deliveries dropped, with
      `MUSTER_MAX_RETRIES=6`; at the default 2 an occasional ETIMEOUT is expected at
      this loss rate. 200 lead calls ran 201 times: one at-least-once duplicate.)*
- [x] AVR footprint reported before/after.
      *(Minimal worker, avr-gcc 7.3 -Os: flash 5772 → 8120 B, static RAM 410 → 468 B,
      for M1 + M2 together. Nano example: RAM 680 → 740 B, flash 11760 → 13822 B.)*

## M2 — Multi-link core  (spec: `MULTILINK_SPEC.md`)
- [x] `muster_add_link()`, per-node routes, broadcast on all links, reassembly keyed by link.
- [x] Placement uses the route link's kbps.
- [x] All seven tests in the spec pass; single-link tests untouched.
      *(`test_multilink.c` for tests 2–6; `test_bus.c` byte-for-byte unchanged.)*
- [ ] AVR with `MUSTER_MAX_LINKS=1`: footprint unchanged (±16 B).
      *(Not measured in isolation: M1 and M2 landed together. By struct arithmetic
      the M2 RAM share with one link is about +13 B (2 B per node row, 1 B per rx
      slot, 3 B in `muster_t`); its flash share is unmeasured.)*

## M3 — SPI link, host side  (spec: `SPI_LINK_SPEC.md`)
- [x] Queue helpers, master/slave `send`/`recv` logic, in-memory wire.
- [x] Two-node runtime test over the SPI wire with payloads above CAN MTU.
- [x] Bit-flip injection test: CRC drops bad packets; calls still complete via retry.
      *(`test_spi_link.c`: 100/100 calls with 10% of packets corrupted each way.)*

## M4 — CAN hardware bring-up
- [ ] Follow `HARDWARE.md` bring-up order; record bus resistance.
- [ ] Verify `muster_link_twai.h` against the installed Arduino-ESP32 core's ESP-IDF
      TWAI API; fix and remove the "not verified" note only after it works.
      *(API checked and compiles against ESP-IDF 4.4 / Arduino-ESP32 2.0.17; hardware pending.)*
- [ ] Same for `muster_link_mcp2515.h` (crystal frequency confirmed).
- [ ] Demo: Nano calls `imu_filter`, gets the correct result; unplug the worker
      mid-call and the job still completes; worker rejoins.
- [ ] Measured: per-hop CALL→RESULT latency (analyser), bus load, failover time.

## M5 — SPI hardware
- [ ] ESP32-S3 master and slave drivers; ATTN line; 4 MHz stable over 10 minutes.
      *(Drivers written and compiling (`*_esp32_spi` envs); stability test needs hardware.)*
- [ ] Lead + worker on CAN **and** SPI; large-payload tasks routed over SPI.
- [ ] Unplug SPI: traffic falls back to CAN with no lost calls.
- [ ] Measured: SPI vs CAN latency and throughput.

## M6 — Typed trampoline generator (`tools/muster-cli`)
- [x] Replace the placeholder registry (`void(*)(void)` casts) with generated
      trampolines using `muster_put_*`/`muster_get_*`.
      *(`muster_gen.h`: struct encoders, trampolines, `muster_setup/loop`, and typed
      call stubs `muster_<task>()` + `_async/_result`. Host round-trip test compiles it
      with `-Werror` and calls every task through its stub.)*
- [x] Supported types: `uint8/16/32_t`, `int8/16/32_t`, `float`, `bool`, and POD structs
      of those (recursive). Fixed-size arrays of those.
- [x] Validator **rejects** `int`, `long`, `unsigned`, `double`, `size_t`, pointers,
      references, with a message naming the fixed-width replacement.
      *(Also `short`, `char`, enums, unions, bit-fields, array parameters, structs with
      bases/virtuals, and calls larger than the smallest board's message buffer.)*
- [x] Generated project for the quadruped example compiles for all three envs with `pio run`.
      *(2026-10-18: nano 698 B RAM / 11.4 KB flash; esp32_a and esp32_b SUCCESS, no
      warnings from generated code. The example's `int sensor_read(int)` became
      `int16_t sensor_read(uint8_t)` — the validator rejected the original.)*
- [x] Analyser moves to libclang (keep the output contract identical).
      *(Original keys kept; new keys added. Parses without a board SDK.)*

## M7 — ~~Local LLM evaluation~~ (superseded, DECISIONS 21)
The local Qwen model was replaced by an assistant connected over MCP (M9). If an
evaluation is wanted for the report: run the assistant over MCP on 20+ test
programs and compare its plans with the baseline (validation pass rate on first
attempt, fewer single-board tasks, lead choice).

## M8 — Telemetry and dashboard
- [ ] Lead emits a binary event stream over USB serial (sequence-numbered, no printf).
- [ ] Host decoder + live dashboard: per-board load, bus load, task placements, failures.

## M9 — MCP server
- [x] Expose `list_boards`, `analyze_source`, `validate_plan`, `generate_project`
      (re-validates internally), `render_wiring`, `deploy` (dry-run unless `confirm=true`).
      *(Plus `baseline_plan` and `build_project`. `muster mcp`, stdio, protocol
      2024-11-05 … 2025-06-18; tested in-process and over a real pipe. Registered for
      Claude Code in `.mcp.json`; not yet exercised from a live client session.)*

## M10 — Shared images and lead-assigned modules  (protocol v3)
One `main.cpp`, one firmware image per board *type*; the lead decides at run time
what each board does. See `PROTOCOL.md` (Modules, ID from a chip number) and
DECISIONS 25–27.
- [x] Core: `uid`-based node IDs with clash detection (`ACK ok=4`), `ASSIGN`,
      module placement and failover, beat-driven reconciliation; compiled out on AVR.
      *(`test_modules.c`: placement, spread, decline, board lost and rejoined, lost
      ASSIGN healed, same-ID clash, ACK for another uid ignored. 130 host checks,
      C99 and C++11. Mutation-checked: removing the clash check, the uid filter or
      the beat resend each fails tests.)*
- [x] AVR budget: minimal worker flash 8120 → 8854 B, static RAM 468 → 492 B (< 600 B).
- [x] Toolchain: `MUSTER_MODULE(...)`, modules in local headers, `shared_images: true`,
      one env per board type, per-port upload, module plan entries validated.
      *(74 toolchain tests. `examples/quadruped_modules` compiles for the lead, the
      shared ESP32 image and the Nano with `pio run`; the old `quadruped` example and
      all five firmware example envs still build.)*
- [ ] Hardware: MAC read on the S3 (`esp_efuse_mac_get_default`), the CAN-ID window
      while two boards share an ID, `muster_link.set_id` on TWAI/MCP2515, module
      failover with real boards. **Nothing here has run on hardware.**

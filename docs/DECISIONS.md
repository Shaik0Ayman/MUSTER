# Decision log

Short records of choices that are easy to second-guess. Add new ones at the bottom.

1. **No pooled memory.** Boards do not share an address space; Muster distributes
   *partitionable work*, not RAM. Two 512 KB boards are not one 1 MB board.
2. **No clock syncing or underclocking.** Every frame carries ordering via call IDs;
   each board runs at full speed. Underclocking the fast board only loses throughput.
3. **Flat topology, not a tree.** CAN is multi-master, so every board is one hop from
   the lead. A tree would turn workers into routers and inflate the port layer.
4. **CAN is the control plane, SPI is an optional accelerator.** CAN shares
   gracefully (arbitration); SPI blocks, so it is point-to-point and dedicated.
5. **Muster uses high CAN IDs** so application frames always win arbitration.
6. **Not an RTOS.** Muster places tasks; the local scheduler (FreeRTOS on ESP32, a
   cooperative loop on AVR) runs them. Placement is not migration: a dispatched task
   runs to completion where it was sent.
7. **Code is resident, placement is live.** An 8-bit AVR cannot load code at run
   time, so every board is flashed with the tasks it may run.
8. **Fixed-width little-endian wire encoding.** `int` and `double` differ between
   AVR and ESP32; raw structs would silently corrupt.
9. **Placement by expected completion time**, not by count of queued calls. A flat
   per-call penalty sent work to an already-busy board in testing.
10. **Lead penalty.** The lead also runs the application and coordinates the bus, so
    running a task locally costs `MUSTER_LEAD_PENALTY`; otherwise the lead (zero
    transfer cost) would always win.
11. **Hub and spoke.** Worker-originated calls and all results go via the lead. Costs
    a hop, buys full visibility for telemetry and a simpler worker.
12. **The LLM proposes, the validator disposes.** Qwen3.5-9B (local, Ollama,
    JSON-schema constrained) suggests the split; a deterministic validator is final.
    A deterministic baseline partitioner is the yardstick the LLM must beat.
13. **Pins only from the curated DB.** The LLM never chooses pins; a hallucinated
    pin is a burnt board.
14. **Single-header runtime** (`MUSTER_IMPLEMENTATION` in exactly one file, generated
    by the toolchain) for painless Arduino/PlatformIO/ESP-IDF integration.
15. **SPI v1 is ESP32↔ESP32 only.** The Nano's one SPI port is already master for
    the MCP2515, and it is 5 V logic.
16. **Duplicates answered from a cache, not prevented.** Exactly-once across
    boards would need cancellation of a call already running on a silent board.
    Instead: a node that is still talking gets the same CALL again (its cache
    answers), a silent one gets the call moved. Exactly-once per board,
    at-least-once across boards.
17. **Heartbeats on a fixed period, on every link.** Suppressing them under load
    let a busy lead starve other workers of beats, and a link that only ever
    carried unicast in one direction would expire.
18. **Reply on the arrival link; demote a route at the first timeout.** A pulled
    SPI cable must not cost a node-timeout of failed retries before CAN takes over.
19. **Lead asks unknown senders to rejoin** (`ACK ok=2`). Without it a restarted
    lead, or a node written off while it could still hear the lead, never came back.
20. **Protocol v2** for the above; frame and message layouts are unchanged.

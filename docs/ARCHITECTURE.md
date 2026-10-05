# Architecture

## Model
- **Lead** holds the node table, makes every placement decision, relays every
  result (hub and spoke — it sees all traffic, which enables the dashboard).
- **Workers** join, execute tasks, report.
- **Code is resident, placement is live.** Each board is flashed with the tasks it
  may run (decided at build time by `muster-cli`); at run time the lead chooses
  among the live boards that hold the task.
- **Several links per board.** CAN is the control plane every board is on; SPI
  is an optional point-to-point accelerator. Unicast takes the fastest link the
  peer was recently heard on; broadcasts and heartbeats go on every link.
- **Static and single-threaded.** No malloc; `muster_poll()` from one context.
- **At-least-once, deduplicated.** Lost frames are recovered by retries; a
  resent call is answered from a cache, so it runs once on any one board.

## Layers inside `muster.h`

```
 application tasks  ←  trampolines (bytes in → typed call → bytes out)
 ─────────────────────────────────────────────────────────────
 logic      lead: node table, placement, pending calls, relay, failover,
                  relay cache
            worker: join, heartbeat, run CALLs (duplicate cache),
                    resend own calls
 routes     per peer: last heard on each link, live links, chosen link
 messages   JOIN · ACK · BEAT · CALL · RESULT          (docs/PROTOCOL.md)
 framing    fragment to link MTU, 2-byte frame header,
            reassembly slots keyed by (link, source)
 ─────────────────────────────────────────────────────────────
 links      muster_link { mtu, kbps, send(), recv(), ctx }  × MUSTER_MAX_LINKS
 transport  TWAI (ESP32 CAN) · MCP2515 (Nano CAN) · SPI (ESP32-S3) · loopback (tests)
```

## The two contracts

```c
typedef int16_t (*muster_task_fn)(const uint8_t *args, uint16_t len,
                                  uint8_t *ret, uint16_t cap);   /* -1 on failure */

typedef struct muster_link {
    uint16_t mtu, kbps;
    int      (*send)(struct muster_link *l, uint8_t dst, const uint8_t *frame, uint16_t len);
    uint16_t (*recv)(struct muster_link *l, uint8_t *src, uint8_t *frame, uint16_t cap);
    void     *ctx;
} muster_link;
```

A link sends and receives **whole frames** and reports the source node ID.
It never interprets the frame contents. `recv` must not block.

A task runs inside `muster_poll()`. It may call `muster_call_async()`, but not
`muster_call()` or `muster_poll()` (both are refused while a task runs).

## Memory model
All state lives in one `muster_t` struct sized by config macros (AVR gets smaller
defaults automatically). The node table is used by the lead only; the
duplicate cache by both (workers: calls they ran; lead: results it relayed).
Frames for other boards are discarded before buffering, so a 2 KB Nano can sit
on a busy bus. Messages are sent straight from header + payload into one
frame buffer at a time; nothing message-sized is copied onto the stack to send.

ATmega328P minimal worker (`make -C firmware/test avr`): 8120 B flash,
468 B static RAM; the deepest receive→reply path sums to roughly 300 B of
stack frames (estimate from `-fstack-usage`, not measured on the chip).

## Toolchain (`tools/muster-cli`)
```
main.cpp + boards.yaml
  → analyse   (tasks, needs, pointer params, globals touched — facts only)
  → partition (Qwen3.5-9B via Ollama, JSON-schema constrained; or deterministic baseline)
  → validate  (final authority: unsafe source, capabilities, RAM; LLM gets ≤3 repairs)
  → generate  (platformio.ini per board, task IDs, residency registry, WireViz wiring)
  → deploy    (pio upload per board, lead last)
```
The generator must emit `muster_set_cost()` on the lead for every task the lead
does not hold, or placement cannot account for work queued on workers.

## Planned
- Hardware bring-up of the CAN and SPI drivers (M4, M5).
- Typed trampoline generator in `muster-cli` (replaces the placeholder registry).
- Telemetry stream from the lead + host dashboard.
- MCP server exposing the toolchain to any MCP-capable assistant.

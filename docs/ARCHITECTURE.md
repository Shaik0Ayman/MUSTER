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
 transport  TWAI (ESP32 CAN) · MCP2515 (Nano CAN) · SPI (ESP32-S3) · UART (any byte stream) · ESP-NOW (ESP32 radio) · loopback (tests) — see LINKS.md
```

## Modules and shared images
A **task** is one call, placed per call. A **module** is a long-running unit
(`int8_t name(uint8_t event)`: START, RUN, STOP) that the lead hands to boards:
`muster_set_module(id, count)` says how many boards run it, `ASSIGN` tells a board
what to run, and the board's heartbeat reports what it runs or declined (START may
return < 0 when its hardware is missing; the lead tries another board). The core
only keeps score; the generated `muster_loop()` starts and stops the code. Modules
need `MUSTER_MAX_MODULES > 0` (default 8 on ESP32, 0 on AVR).

With `shared_images: true` all boards of a type that can read a chip number (and are
not the lead) are one firmware image: the code for every task and module any of them
may hold is in it, and the lead decides at run time. See `PROTOCOL.md` for the wire
side.

## The two contracts

```c
typedef int16_t (*muster_task_fn)(const uint8_t *args, uint16_t len,
                                  uint8_t *ret, uint16_t cap);   /* -1 on failure */

typedef struct muster_link {
    uint16_t mtu, kbps;
    int      (*send)(struct muster_link *l, uint8_t dst, const uint8_t *frame, uint16_t len);
    uint16_t (*recv)(struct muster_link *l, uint8_t *src, uint8_t *frame, uint16_t cap);
    void     *ctx;
    void (*set_id)(struct muster_link *l, uint8_t id);  /* optional, may be NULL */
} muster_link;
```

A link sends and receives **whole frames** and reports the source node ID.
`set_id` is called if the board's node ID changes after the link started (CAN puts
it on the wire).
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
  → analyse   (libclang: tasks, wire types, encoded sizes, globals touched — facts only)
  → partition (deterministic baseline, a plan file, or an assistant over MCP)
  → validate  (final authority: non-portable types, globals, capabilities, RAM,
               message size on the smallest board, node/task limits)
  → generate  (muster_gen.h: struct encoders, trampolines, muster_setup/loop, typed
               call stubs; platformio.ini per board; runtime headers; WireViz wiring)
  → build     (pio run per board)
  → deploy    (pio upload per board, lead last)
```
`muster mcp` exposes each step as an MCP tool, so an assistant (Claude Code or any
MCP client) makes the partitioning judgement; `generate_project` re-validates and
`deploy` is a dry run unless confirmed. The generated setup calls
`muster_set_cost()` for every task on every board, so the lead can price tasks it
does not hold.

## Planned
- Hardware bring-up of the CAN and SPI drivers (M4, M5).
- SPI accelerator links generated from the board DB.

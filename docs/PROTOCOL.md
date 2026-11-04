# Muster wire protocol — version 3

Source of truth is `firmware/src/muster.h`. If you change bytes on the wire,
bump `MUSTER_PROTO_VERSION` and update this file in the same commit.

All multi-byte fields are **little-endian, fixed width**. Floats are IEEE-754
binary32. Never send native `int`, `long`, `double`, or raw structs.

**Changes from v2:** `JOIN` gained `uid u32, modules u8` (19 bytes in all); `ACK`
gained `uid u32` and code 4 (ID clash); `BEAT` gained `running u8, refused u8`;
new message `ASSIGN` (6). The lead can now hand long-running *modules* to
boards, and a worker may derive its node ID from a chip-unique number. Frame
layouts are unchanged; a v2 and a v3 board refuse each other at `JOIN`.

**Changes from v1:** `ACK` gained codes 2 (rejoin) and 3 (table full);
heartbeats are sent on a fixed period on every link; workers resend unanswered
calls; duplicate calls are answered from a cache instead of re-run; a board may
use several links. Frame and message layouts are unchanged; a v1 and a v2 board
refuse each other at `JOIN`.

## Layer 1 — link frames

Each link carries frames of at most `link->mtu` bytes. Every frame starts with a
2-byte header so any board can drop frames for others *before* buffering them.

| Byte | Field | Notes |
|---|---|---|
| 0 | `dst` | destination node ID, `0xFF` = broadcast |
| 1 | `idx \| last<<7` | fragment index 0–127, bit 7 set on the final fragment |
| 2.. | payload | up to `mtu - 2` bytes of the message |

A message that fits in one frame is sent with `idx = 0, last = 1` (fast path).
Fragments of one message travel on one link, in order. The receiver
reassembles per `(link, source)`; a gap, an overflow of `MUSTER_MAX_MSG`, or
`MUSTER_CALL_TIMEOUT_MS` without the next fragment discards the partial message
(the sender's retry handles recovery). A new `idx = 0` from the same source on
the same link restarts reassembly.

A link's MTU must be 3..`MUSTER_MAX_FRAME` (default 64); `muster_add_link()`
refuses others.

### CAN mapping (`muster_link_twai.h`, `muster_link_mcp2515.h`)
- 29-bit extended ID = `MUSTER_CAN_BASE (0x1F000000) | src_node`.
- On CAN the lower ID wins arbitration. Muster's IDs (base ID `0x7C0`) lose to
  every extended ID below `0x1F000000` and to standard 11-bit IDs `0x000–0x7C0`.
  An application using standard IDs `0x7C1–0x7FF` would lose to Muster; keep
  application IDs below that.
- Receivers ignore any ID outside `MUSTER_CAN_BASE` (mask `0x1FFFFF00`), so Muster
  can share a bus with an existing application. The destination is in the frame
  data, not the ID, so CAN acceptance filters can only separate Muster traffic
  from application traffic, not by destination.
- 500 kbit/s, classic CAN, MTU 8 → 6 payload bytes per frame.

### SPI mapping
See `SPI_LINK_SPEC.md`. SPI adds its own sync byte, length and CRC-16 around
each frame; MTU 60.

## Layer 2 — messages

Header (3 bytes): `type u8, src u8, dst u8`.

| Type | ID | Direction | Body |
|---|---|---|---|
| `JOIN` | 1 | worker → broadcast | `ver u8, cores u8, flags u8, ram_kb u16, clock_mhz u16, resident u32, uid u32, modules u8` |
| `ACK` | 2 | lead → worker | `ver u8, ok u8, uid u32` (see codes below) |
| `BEAT` | 3 | worker → lead; lead → broadcast | `running u8, refused u8` (the lead sends 0, 0) |
| `CALL` | 4 | worker → lead (to place); lead → worker (to run) | `origin u8, call_id u8, task u8, args…` |
| `RESULT` | 5 | worker → lead; lead → origin | `call_id u8, status i8, ret…` |
| `ASSIGN` | 6 | lead → worker | `want u8` |

- `flags`: bit0 FPU, bit1 Wi-Fi, bit2 BLE.
- `resident`: bit *n* set ⇔ task ID *n* is flashed on that board. The lead
  ignores bits at or above its `MUSTER_MAX_TASKS`.
- `status`: `0` OK, `-1` ERR, `-2` ENOHOST, `-3` ETIMEOUT, `-4` ETASK. Any
  positive value received is treated as ERR.
- `ACK ok`: `0` refused (protocol version mismatch), `1` accepted, `2` rejoin
  (the lead does not know you), `3` refused (node table full), `4` ID clash
  (another live board with a different `uid` already uses your node ID).
- `uid`: tells apart two boards that briefly share a node ID. A board with a
  fixed ID uses `uid = id`; one that derives its ID from a chip number (the low
  4 bytes of an ESP32 MAC) uses that number. An `ACK` is acted on only if its
  `uid` equals the receiver's own; `0xFFFFFFFF` means "any board" and is used
  for `ok=0` and `ok=2`, which the lead sends without knowing the sender's uid.
- `modules` / `want` / `running` / `refused`: 8-bit masks, bit *k* = module *k*.
  `modules` = flashed on that board; `want` = the full set the lead wants it to
  run; `running` = what it runs now; `refused` = modules it declined to start.
- `args` is at most `MUSTER_MAX_MSG - 6` bytes, `ret` at most `MUSTER_MAX_MSG - 5`.
  A result that does not fit is sent as `status = ERR` with no data.

## Flows

**Join.** A worker broadcasts `JOIN` every `MUSTER_HEARTBEAT_MS` until it gets
`ACK ok=1`. The lead answers every `JOIN` (repeats are harmless):
- wrong `ver` → `ACK ok=0`, event `VERSION`;
- table full → `ACK ok=3`, event `REFUSED` (a full table recycles the row of a
  node that is currently lost);
- otherwise records the node's capabilities, `ACK ok=1`, event `JOINED` if new.

A worker clears its duplicate cache whenever it (re)joins: call IDs restart
with a new lead.

**ID from a chip number.** A worker that uses `muster_set_uid()` takes node ID
`1 + CRC-16(uid[4] ‖ salt) % 199` (IDs 1..199; 0 is the lead, 200..253 are left
for boards with a fixed ID, which cannot move out of a clash; `0xFE`/`0xFF` are
reserved), `salt` starting at 0. If the lead already has a
*live* node with that ID and a different `uid`, it answers the later `JOIN` with
`ACK ok=4` (event `REFUSED`) and leaves its table alone. The newcomer increments
`salt`, takes the new ID, tells its links (`muster_link.set_id`, because CAN puts
the ID in the arbitration field) and sends `JOIN` again. Until the clash is
resolved the two boards use the same CAN identifier; that window and the
resulting bus errors have **not been observed on hardware**.

**Modules.** A module is a long-running unit (start / run / stop in the
application), unlike a task, which is one call. The lead holds `count` per
module and keeps `want` per node:
- a module stays on its board while that board is alive and has not declined
  it; only a shortfall or surplus is moved (state does not move);
- a new copy goes to the live, non-declining board holding the module with the
  fewest assigned modules (the lead counts as one busier), then most RAM, then
  lowest ID;
- every change sends `ASSIGN` with the board's whole `want` set;
- every worker `BEAT` carries `running`/`refused`; if `running` differs from
  `want & ~refused` the lead sends `ASSIGN` again, so a lost `ASSIGN` heals
  within one heartbeat;
- a worker whose lead is lost treats its `want` as empty. A (re)joining board
  starts with nothing assigned.
Events (lead): `MODULE_START` (module, node), `MODULE_STOP`.

**Rejoin.** If the lead receives anything other than `JOIN` from a node it does
not have as alive (the lead restarted, or it wrote the node off as lost while
the node could still hear it), it answers `ACK ok=2`. The worker drops to
unjoined and sends `JOIN` on its next poll.

**Liveness.** Every `MUSTER_HEARTBEAT_MS` the lead broadcasts `BEAT` and each
joined worker sends `BEAT` to the lead — always, on **every link**, however busy
the board is with other traffic. Any message from a peer counts as hearing it.
A link to a peer is dropped after `MUSTER_NODE_TIMEOUT_MS` without hearing that
peer on it; a peer with no live link is lost (lead: event `LOST`; worker: its
lead is gone, so it rejoins and its pending calls fail with `ETIMEOUT`).

**Call from the lead.** The lead places the task. If the lead wins, it runs the
task locally. Otherwise it sends `CALL` (origin = lead, its own `call_id`) and the
worker answers `RESULT` on the link the `CALL` arrived on.

**Call from a worker.** The worker sends `CALL` to the lead. The lead places it,
forwards `CALL` with its own `call_id`, receives `RESULT`, and relays it to the
origin with the origin's `call_id`. If no answer arrives within
`MUSTER_CALL_TIMEOUT_MS`, the worker resends the same `CALL` (same `call_id`),
up to `MUSTER_MAX_RETRIES` times; it gives up after `MUSTER_CALL_DEADLINE_MS`.
The lead treats a resend as follows:
- still being worked on → ignored;
- recently finished → the cached `RESULT` is sent again (not re-run);
- otherwise → placed as a new call.

**Failover.** For each call it placed, the lead waits `MUSTER_CALL_TIMEOUT_MS`
(or until the node is lost), then retries, up to `MUSTER_MAX_RETRIES` times:
- the node has been heard since the `CALL` went out → the `CALL` or `RESULT`
  was lost, not the board: resend to the **same** node. If the node was silent
  on its route link but heard on another, that link is dropped first (a pulled
  SPI cable falls back to CAN at the first timeout);
- the node has been silent → re-place on the best board **not yet tried**; if
  every holder has been tried, try the best live holder again.

A `RESULT` from any board the call was sent to completes it; later ones are
ignored.

**Duplicate suppression.** A worker caches the last `MUSTER_DEDUPE` calls it
ran, keyed by `(sender, call_id, task, CRC-16 of args)`. A `CALL` matching an
entry is answered with the cached status and result and the task is **not**
run again. The lead keeps the same cache for results it relayed to workers.
Guarantee: a resend to the same board runs the task once. A call moved to a
*different* board (because the first went silent, e.g. stuck in a long task)
can still run twice: execution is at-least-once across boards.

## Multi-link routing

- A board may have up to `MUSTER_MAX_LINKS` links (AVR default 1, else 2).
- For each peer, the core tracks when it was last heard on each link and which
  links are live (`links_seen`). Unicast goes over the live link with the
  highest `kbps` (`route`). With no live link known, a message goes out on all
  links.
- `JOIN`, `BEAT` and other broadcasts go out on every link.
- Replies to a `CALL` (`RESULT`, or an immediate `ERR`/`ENOHOST`) go back on the
  link the `CALL` arrived on.

## Placement (lead)

Lower score wins:

```
score = (queued_cost_on_node + task_cost) * 1000 / clock_mhz
      + bytes * 8000 / route_link_kbps      (0 if the lead runs it itself)
      + MUSTER_LEAD_PENALTY                 (only if the lead runs it itself)
```

`task_cost` is the lead's `cost[task]`: set by `muster_register_task` for tasks
the lead holds and by `muster_set_cost` for those it does not (unknown = 1).
`bytes` is the argument length.

## Timing defaults

| Macro | Default |
|---|---|
| `MUSTER_HEARTBEAT_MS` | 200 |
| `MUSTER_NODE_TIMEOUT_MS` | 1000 |
| `MUSTER_CALL_TIMEOUT_MS` | 400 |
| `MUSTER_MAX_RETRIES` | 2 |
| `MUSTER_CALL_DEADLINE_MS` | `CALL_TIMEOUT × (2 × MAX_RETRIES + 2)` = 2400 |
| `MUSTER_LEAD_PENALTY` | 3000 |

A task that runs longer than `MUSTER_CALL_TIMEOUT_MS` is retried on its own
board (answered from the cache once it finishes) or moved; one longer than
`MUSTER_NODE_TIMEOUT_MS` gets its board declared lost. Raise both for long tasks.

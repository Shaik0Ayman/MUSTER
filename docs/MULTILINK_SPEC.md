# Multi-link core — spec (ROADMAP M2)

**Status: implemented** in `muster.h`, tested in `firmware/test/test_multilink.c`
(spec tests 2–6), `test_bus.c` unchanged (test 1), `make avr` (test 7).
Implementation notes and where it differs from the text below:
- Per-peer state is a `muster_route { seen[MUSTER_MAX_LINKS], links_seen, route }`
  used both for nodes (lead) and for the lead (`lead_rt`, worker). RAM cost:
  `4 × MUSTER_MAX_LINKS + 2` bytes per peer. With one link this replaces the old
  `last_seen` (4 B), so a node grows by 2 B.
- Heartbeats from workers now go on every link on a fixed period (not only when
  idle) — otherwise a link a worker never unicasts on would expire.
- On a call timeout, if the node was heard on another link but not on its route
  link since the CALL went out, the route link is dropped immediately (not after
  `MUSTER_NODE_TIMEOUT_MS`). A worker answers a CALL on the link it arrived on.
  Together these make "unplug SPI mid-call" complete at the first retry.

Goal: one board can be attached to several links at once (e.g. CAN to everyone
plus a fast SPI link to one neighbour). The core routes each message over the
best link to its destination and prices placement per link.

## Design rules
- CAN is the **control plane**: every board is on it; it is always shareable.
- SPI is an **optional accelerator**: point-to-point, dedicated peripheral, used
  for bulk payloads between two boards that have it.
- Losing the SPI link must degrade to CAN automatically, never break the system.

## API changes
```c
#ifndef MUSTER_MAX_LINKS
#  define MUSTER_MAX_LINKS (defined(__AVR__) ? 1 : 2)   /* write as #if blocks in C99 */
#endif
int  muster_add_link(muster_t *m, muster_link *link);    /* returns link index or -1 */
```
`muster_begin()` keeps its signature; its `link` argument becomes link 0.
Existing single-link code and tests must keep working unchanged.

## Routing
- Each `muster_node` gains `uint8_t links_seen` (bitmask of link indices this node
  has been heard on) and `uint8_t route` (index of the link to use).
- On every received message, set the bit for the arrival link in the sender's
  `links_seen`, then set `route` to the seen link with the highest `kbps`.
- A link's bit is cleared for a node if nothing arrives from that node on that
  link for `MUSTER_NODE_TIMEOUT_MS` (track `last_seen_on[link]` per node, or a
  cheaper equivalent — justify the RAM cost in the PR).
- Workers need the same for the lead (`lead_route`).
- **Unicast** goes out on the node's `route` link only.
- **Broadcast** (`JOIN`, lead `BEAT`) goes out on **all** links.
- `muster_poll()` drains all links.

## Reassembly
Slots are keyed by `(link_index, src)`, not `src` alone — fragments of two
different messages from one source can interleave across links.

## Placement
Replace the single `m->link->kbps` with the kbps of the route link to that node:
```
transfer = bytes * 8000 / links[node.route]->kbps
```
So a data-heavy task prefers the board reachable over SPI.

## Frame buffer
The core currently hard-codes 64-byte frame buffers. Make this
`MUSTER_MAX_FRAME` (default 64; must be ≥ every attached link's MTU), and refuse
`muster_add_link()` for a link whose MTU exceeds it.

## Tests required (host, loopback)
Build a rig with two loopback buses: bus A (MTU 8, 500 kbps, all three nodes)
and bus B (MTU 64, 4000 kbps, lead + worker 1 only).
1. All single-link tests still pass untouched.
2. Lead learns that worker 1 is reachable on both links and routes to bus B.
3. A large-payload task (≥ 48 bytes) is placed on worker 1 in preference to an
   otherwise identical worker reachable only over CAN.
4. Unplug bus B: calls to worker 1 fall back to CAN and still complete correctly;
   no node is declared lost (it is still alive on CAN).
5. Replug bus B: route returns to bus B.
6. Broadcasts reach a node that exists only on bus A.
7. `make avr` footprint delta reported; AVR build with `MUSTER_MAX_LINKS=1` unchanged.

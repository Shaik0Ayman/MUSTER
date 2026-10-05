# SPI link — spec (ROADMAP M3, M5)

`firmware/src/muster_link_spi.h` implements a `muster_link` over SPI between
**two ESP32-S3 boards**.

**Status:** framing, queues, master/slave logic and an in-memory wire are done
and host-tested (`test_spi_frame.c`, `test_spi_link.c`: tests 1–3 below). The
ESP32-S3 drivers are written against ESP-IDF 4.4 (bundled with Arduino-ESP32
2.0.17) and compile (`pio run -e lead_esp32_spi -e worker_esp32_spi`), but are
**not verified on hardware** (M5). Differences from the sketch below:
- `muster_spi_master_begin` / `muster_spi_slave_begin` also take an
  `mspi_esp32 *` holding the driver state and the word-aligned DMA buffers.
- The slave keeps one transaction queued and re-queues it from `send`/`recv`,
  i.e. from `muster_poll()`. The slave board must poll faster than
  `MSPI_POLL_MS`, or the master clocks an empty slave and that packet is lost
  (CRC drop, core retry).
- A frame the slave queues while a poll packet is already loaded goes out one
  transaction later (ATTN makes the master come straight back).

## Scope for v1: ESP32 ↔ ESP32 only
The Nano is out of scope for SPI v1, deliberately:
- its single hardware SPI is already the **master** for the MCP2515 CAN module, so
  it cannot also be an SPI **slave** to an ESP32;
- it is 5 V logic and the ESP32-S3 is 3.3 V — a level shifter would be required.
Document this; do not try to make it work in v1.

## Signals
| Signal | Direction | Notes |
|---|---|---|
| SCK, MOSI, MISO, CS | master → slave (MISO back) | dedicated SPI host, not shared with anything |
| ATTN | slave → master, active low | slave pulls low when it has frames to send |
| GND | common | mandatory |

Pins come from the board DB (`boards.yaml`); add an `spi_link` entry there.
Keep wires under ~15 cm. Start at 4 MHz on jumpers; raise only after it is stable.

## Why the protocol looks like this
SPI is master-clocked: the slave can never speak first. And unlike CAN it has
**no hardware CRC**, so corruption must be detected in software.

## Transaction format
Every transaction is a **fixed size**, full duplex: `N = 4 + MSPI_MAX_PAYLOAD`
bytes. Both sides transmit one packet and receive one packet at the same time.

```
[0xA5 sync][len u8][payload … len bytes][pad to MSPI_MAX_PAYLOAD][crc16 LE]
```
- `len = 0` means "nothing to send" (a poll). Its CRC is still valid.
- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over `len` and the `len`
  payload bytes. Check value: CRC of ASCII `"123456789"` is `0x29B1`.
- A packet with bad sync, `len > MSPI_MAX_PAYLOAD`, or bad CRC is **dropped
  silently** and counted (`crc_errors`). The core's call timeout handles recovery.
- `payload` is exactly one Muster link frame (the 2-byte frame header + data),
  so the link MTU is `MSPI_MAX_PAYLOAD` (default 60, so N = 64).

## Behaviour
**Master**
- `send()`: queue the frame, then immediately run one transaction carrying it.
  Whatever the slave returns in the same transaction goes to the RX queue.
- `recv()`: return a queued RX frame if any. Otherwise, if ATTN is low **or** at
  least `MSPI_POLL_MS` (default 2 ms) has passed since the last transaction, run a
  poll transaction (`len = 0`) and queue anything returned.

**Slave**
- Always keeps one transaction queued in the SPI slave driver so the master can
  clock at any time.
- `send()`: put the frame in a TX queue (depth `MSPI_TXQ`, default 4); load the head
  into the next queued transaction; assert ATTN while the queue is non-empty.
  Return -1 if the queue is full.
- `recv()`: return frames the master delivered.

**Source ID.** Point-to-point, so `recv()` reports the configured peer node ID.

## Tests required
1. Host: CRC check value; pack/unpack round trip; corrupted byte rejected;
   `len` overflow rejected; poll packet round trip.
2. Host: an in-memory "wire" that runs master/slave transactions, used as a
   `muster_link` in a two-node runtime test (payloads larger than CAN MTU, no
   fragmentation needed up to 58 bytes of message).
3. Host: random bit-flip injection — the runtime still completes calls via retry.
4. Hardware (M5): throughput and per-hop latency vs CAN, logged with the logic
   analyser; placement prefers the SPI-reachable board for large payloads.

## Implementation notes to verify, not assume
- ESP-IDF provides `driver/spi_master.h` and `driver/spi_slave.h`; the slave API
  requires transactions to be queued in advance. Check the API against the
  ESP-IDF version bundled with the installed Arduino-ESP32 core before coding.
- DMA-capable buffers may need `heap_caps_malloc(..., MALLOC_CAP_DMA)` or
  `WORD_ALIGNED_ATTR` static buffers — prefer static aligned buffers (no malloc rule).

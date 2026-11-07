# Links (transports)

A **link** is how one board's frames reach other boards. The runtime does not care
what is underneath: it hands a link a frame of at most `mtu` bytes and asks it for
received frames, and the core handles everything else (fragmentation, retries,
duplicate suppression, routing across several links, liveness).

Every link is one header in `firmware/src/`. Include the ones a board uses after
`muster.h`, and attach each one to the runtime.

| Link | Header | Medium | Topology | MTU | `kbps` (placement) | Integrity | Platforms | Status |
|---|---|---|---|---|---|---|---|---|
| Loopback | `muster_link_loopback.h` | memory | shared bus | any (8 = classic CAN) | any | none needed | host | host tests only, by design |
| TWAI | `muster_link_twai.h` | CAN 2.0B, 29-bit IDs | shared bus | 8 | 500 | CAN hardware CRC | ESP32, ESP32-S3 | compiles; **not run on hardware** |
| MCP2515 | `muster_link_mcp2515.h` | CAN over an SPI controller | shared bus | 8 | 500 | CAN hardware CRC | Arduino Nano/Uno (AVR) | compiles; **not run on hardware** |
| SPI | `muster_link_spi.h` | SPI + an ATTN line | point to point | 60 | set at attach | CRC-16/CCITT-FALSE | ESP32-S3 | logic host-tested; drivers compile; **not run on hardware** |
| UART | `muster_link_uart.h` | any byte stream | point to point | 32 | set at attach | CRC-16 | anything with a serial port | host-tested; **worked on hardware** (2026-11-07; latency not measured) |
| ESP-NOW | `muster_link_espnow.h` | 2.4 GHz radio, broadcast | shared medium | 64 | 1000 | 802.11 FCS | ESP32 family (Arduino-ESP32) | host-tested; **ran on hardware** (C6 + 2 DevKits, 2026-11-07): join, calls, failover, rejoin |

"Not run on hardware" is literal. UART and ESP-NOW have run on real boards (results
and their limits: [`TESTING.md`](TESTING.md#hardware-results)); CAN and SPI have not,
so what this file says about them comes from the code and the host simulations. If you
bring a link up, please open a *Hardware report* issue.

## Choosing a link

* **Boards on one wire, mixed families (AVR + ESP32):** CAN. TWAI on ESP32s,
  MCP2515 on boards with no CAN controller; both use the same ID scheme and share
  one bus. See [`HARDWARE.md`](HARDWARE.md) for transceivers and termination.
* **Two ESP32-S3s that need more bandwidth than CAN:** SPI, alongside CAN if you
  like. See [`SPI_LINK_SPEC.md`](SPI_LINK_SPEC.md) and [`MULTILINK_SPEC.md`](MULTILINK_SPEC.md).
* **ESP32 boards with no transceivers at all:** ESP-NOW. No wiring, just power.
* **Anything with a serial port, or a simulator:** UART. One link per cable.
* **Tests:** loopback.

A board can hold up to `MUSTER_MAX_LINKS` links (1–8). The core learns which link
reaches which peer and sends unicast frames over the fastest live one; placement
prices data transfers by each link's `kbps`.

## Attaching a link

`muster_begin()` takes the first link. Each further link is added with
`muster_add_link()` (raise `MUSTER_MAX_LINKS` before including `muster.h`).
`muster_add_link()` refuses a link whose MTU is ≤ 2 or above `MUSTER_MAX_FRAME`.

```c
#define MUSTER_MAX_LINKS 2
#define MUSTER_IMPLEMENTATION
#include "muster.h"
#include "muster_link_twai.h"
#include "muster_link_uart.h"

static muster_t m;
static muster_link can, uart;
static muster_uart_ctx uart_ctx;

void setup(void) {
    muster_twai_begin(&can, NODE_ID, TX_PIN, RX_PIN);        /* pins from boards.yaml */
    muster_begin(&m, NODE_ID, 0, cores, ram_kb, mhz, caps, &can, millis_fn);
    muster_uart_attach(&uart, &uart_ctx, &Serial1, uart_write, uart_read,
                       /*peer=*/3, /*kbps=*/115);
    muster_add_link(&m, &uart);
}
```

A link is a struct of four function pointers and a context (see `muster_link` in
`muster.h`): `send(dst, frame, len)`, `recv(&src, frame, cap)` (never blocks; 0 =
nothing), and an optional `set_id(id)` for links that carry the node ID themselves
(CAN IDs, the ESP-NOW packet header). Writing a new link means filling those in.

## Loopback — `muster_link_loopback.h`

An in-memory broadcast bus for host tests. Every frame reaches every other port;
frames are capped at the port's MTU. Fault injection: `unplugged` and `muted`
ports, a random drop rate (`drop_permille`, seeded) and a per-frame drop hook. Used by
every test in `firmware/test/`.

## CAN — `muster_link_twai.h`, `muster_link_mcp2515.h`

* `muster_twai_begin(&link, node_id, tx_pin, rx_pin)`: ESP32 on-chip TWAI, needs an
  SN65HVD230 (3.3 V) transceiver.
* `muster_mcp2515_begin(&link, &ctx, &mcp, node_id)`: an MCP2515 + TJA1050 module
  through the `autowp-mcp2515` library. Set `MUSTER_MCP_CLOCK` to the module's
  crystal (most are 8 MHz).

Muster frames use 29-bit extended IDs from `MUSTER_CAN_BASE` (0x1F000000) upward, with
the source node in the low byte. Low IDs win arbitration on CAN, so an application
sharing the bus beats Muster as long as it stays below that range; frames outside
it are ignored. The bit rate is 500 kbit/s. Wire format: [`PROTOCOL.md`](PROTOCOL.md).

## SPI — `muster_link_spi.h`

Point-to-point between two ESP32-S3s, full duplex, fixed-size packets
`[0xA5][len][payload][pad][crc16]`, with an ATTN line so the slave can ask to be
polled. `muster_spi_master_begin()` / `muster_spi_slave_begin()` take the pins and the
clock. The full design is in [`SPI_LINK_SPEC.md`](SPI_LINK_SPEC.md).

## UART — `muster_link_uart.h`

A point-to-point link over any byte stream: a hardware UART, SoftwareSerial, a USB
CDC port, a pipe in a test.

```c
static void uart_write(void *io, const uint8_t *p, uint16_t n) { ((HardwareSerial *)io)->write(p, n); }
static int  uart_read(void *io) { return ((HardwareSerial *)io)->read(); }   /* -1 when empty */

muster_uart_attach(&link, &ctx, &Serial1, uart_write, uart_read, peer_node_id, kbps);
```

* **Frame:** `0x7E | len | payload[len] | crc16 lo | crc16 hi`, with the CRC over
  `len` and the payload. No byte stuffing.
* **Errors:** a bad length or CRC drops the frame and the receiver hunts for the next
  `0x7E`. A corrupted byte costs one frame and the runtime's retries recover it. This
  is tested with injected corruption in `test_uart_link`.
* **Point to point:** the far end is fixed (`peer`), so a lead with *n* workers needs
  *n* UART links (and `MUSTER_MAX_LINKS` ≥ *n*).
* **Settings:** `MUSTER_UART_MTU` (default 32; ≤ 253 and ≤ `MUSTER_MAX_FRAME`, checked
  at compile time). Pass the line rate / 1000 as `kbps`, e.g. 115 for 115200 baud.
* `recv` reads whatever bytes are waiting and returns at once; it never blocks.

## ESP-NOW — `muster_link_espnow.h`

Muster over the ESP32's connectionless WiFi radio. All boards broadcast on one
channel, so to the runtime it looks like a CAN bus: every board hears every frame,
and the core drops frames addressed to someone else.

```c
static muster_link link;
static mespnow_ctx radio;

/* worker: a plain station on the shared channel */
muster_espnow_begin(&link, &radio, node_id, /*channel=*/1, /*ap_running=*/0);

/* lead that also serves a page: start the AP on the same channel first */
WiFi.mode(WIFI_AP_STA);
WiFi.softAP("ssid", "password", 1);
muster_espnow_begin(&link, &radio, 0, 1, /*ap_running=*/1);
```

* **Packet:** `src node ID (1 byte) | Muster frame (≤ MUSTER_ESPNOW_MTU)`. ESP-NOW
  only reports the sender's MAC, so the node ID travels in the packet; `set_id` keeps
  it current when the node ID changes (`muster_set_uid`, ID clash).
* **Delivery:** broadcast frames are not acknowledged by the radio. The runtime's
  retries and duplicate cache do the recovering; `test_espnow_link` runs a lead and
  two workers at 10 % packet loss.
* **Threading:** the receive callback runs in the WiFi task. It only pushes into a
  single-producer / single-consumer ring (memory barriers, no locks), and
  `muster_poll()` pops from it, so the core stays single-threaded. A full ring drops
  the frame and counts it in `radio.dropped`. `radio.sent` / `radio.send_failed`
  count transmissions.
* **Settings:** `MUSTER_ESPNOW_MTU` (default 64; ≤ 249 and ≤ `MUSTER_MAX_FRAME`),
  `MUSTER_ESPNOW_RING` (default 16 frames; 2–255). Both are checked at compile time.
* **Channel:** every board must be on the same channel. A lead that runs an access
  point puts the AP on that channel and the radio stays there. A board that also
  joins a WiFi network as a station follows that router's channel, so its peers
  must match it.
* **One link per board:** the ESP-NOW receive callback has no user pointer, so the
  header keeps one global context.
* **Platforms:** the radio code compiles only for Arduino-ESP32 (`ARDUINO_ARCH_ESP32`),
  2.0.x and 3.x. Elsewhere, including Arduino AVR builds and host tests, only the
  portable ring and packet code is compiled (`make -C firmware/test guards` checks this).
* **Security:** like the CAN bus, ESP-NOW frames here are neither authenticated nor
  encrypted, and anyone in radio range on that channel can join. See
  [`../SECURITY.md`](../SECURITY.md).

Worked example (run on hardware): [`firmware/examples/trio`](../firmware/examples/trio/README.md), three
ESP32s with a live dashboard.

# Links

A link is how one board's messages get to the others. The runtime doesn't care what's
underneath. It hands a link a frame of at most `mtu` bytes and asks it for anything it
has received. Everything else lives in the core: splitting and reassembling messages,
retries, duplicate suppression, routing across several links, and noticing when a board
has gone quiet.

Each link is a single header in `firmware/src/`. You include the ones a board uses after
`muster.h` and attach them to the runtime.

| Link | Header | Medium | Shape | MTU | `kbps` | Integrity check | Runs on | Status |
|---|---|---|---|---|---|---|---|---|
| Loopback | `muster_link_loopback.h` | memory | shared bus | any (8 = classic CAN) | any | not needed | host, or inside one chip | used by every test |
| TWAI | `muster_link_twai.h` | CAN 2.0B, 29-bit IDs | shared bus | 8 | 500 | CAN hardware CRC | ESP32, ESP32-S3 | compiles; not yet run on hardware |
| MCP2515 | `muster_link_mcp2515.h` | CAN through an SPI controller | shared bus | 8 | 500 | CAN hardware CRC | Arduino Nano/Uno and other boards with SPI | compiles; not yet run on hardware |
| SPI | `muster_link_spi.h` | SPI plus an ATTN line | point to point | 60 | you set it | CRC-16/CCITT-FALSE | ESP32-S3 | logic host-tested; drivers compile; not yet run on hardware |
| UART | `muster_link_uart.h` | any byte stream | point to point | 32 | you set it | CRC-16 | anything with a serial port | host-tested; worked on hardware (2026-11-07), latency not measured |
| ESP-NOW | `muster_link_espnow.h` | 2.4 GHz radio, broadcast | shared medium | 64 | 1000 | 802.11 checksum | ESP32 family (Arduino-ESP32) | host-tested; ran on hardware (C6 + 2 DevKits, 2026-11-07): join, calls, failover, rejoin |

When we say "not yet run on hardware" we mean it literally. UART and ESP-NOW have run on
real boards ([results](TESTING.md#hardware-results)). CAN and SPI haven't, so what this
page says about them comes from the code and the simulations. If you bring one of them
up, please open a *Hardware report* issue and tell us how it went.

## Which one should I use?

If your boards are a mix of families on one wire (say an AVR and some ESP32s), use CAN.
The ESP32s use TWAI and the boards without a CAN controller use an MCP2515. Both use the
same ID scheme, so they share one bus happily. [`HARDWARE.md`](HARDWARE.md) covers
transceivers and termination.

If two ESP32-S3s need more bandwidth than CAN gives you, add an SPI link between them,
next to CAN if you like. [`SPI_LINK_SPEC.md`](SPI_LINK_SPEC.md) and
[`MULTILINK_SPEC.md`](MULTILINK_SPEC.md) explain how that works.

If your boards are all ESP32s and you'd rather not wire anything, use ESP-NOW. They only
need power.

If all you have is a serial port, or you're working in a simulator, use UART, with one
link per cable.

For tests, use loopback.

A board can have up to `MUSTER_MAX_LINKS` links (between 1 and 8). The core keeps track of
which link reaches which board, and sends each message over the fastest one that's still
working. When it estimates how long a call will take, it uses each link's `kbps` to price
the data transfer.

## Attaching a link

`muster_begin()` takes the first link. You add more with `muster_add_link()`, after raising
`MUSTER_MAX_LINKS` before you include `muster.h`. `muster_add_link()` refuses a link whose
MTU is 2 or less, or larger than `MUSTER_MAX_FRAME`.

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

Underneath, a link is just a struct with four function pointers and a context (see
`muster_link` in `muster.h`). `send(dst, frame, len)` sends a frame. `recv(&src, frame, cap)`
returns a received frame, or 0 if there's nothing, and must never block. The optional
`set_id(id)` is for links that carry the node ID themselves (CAN IDs, the ESP-NOW packet
header), so they hear about it when the ID changes. Writing a new link means filling
these in.

## Loopback

This is an in-memory broadcast bus for tests. Every frame reaches every other port, and
frames are capped at the port's MTU. You can unplug or mute a port, drop a share of
deliveries at random (the random seed is fixed, so failures can be replayed), or drop
particular frames with a hook. Every test in `firmware/test/` uses it.

Its sizes (`LB_MAX_NODES`, `LB_QUEUE`, `LB_FRAME`) suit the host and add up to about
135 KB. You can override them, which is how
[`examples/portability`](../firmware/examples/portability) fits a whole bus into a 2 KB Nano.

## CAN

There are two drivers:

- `muster_twai_begin(&link, node_id, tx_pin, rx_pin)` uses the ESP32's on-chip TWAI
  controller. It needs an SN65HVD230 (3.3 V) transceiver.
- `muster_mcp2515_begin(&link, &ctx, &mcp, node_id)` uses an MCP2515 + TJA1050 module
  through the `autowp-mcp2515` library. Set `MUSTER_MCP_CLOCK` to match the crystal on your
  module; most have an 8 MHz one.

Muster uses 29-bit extended IDs starting at `MUSTER_CAN_BASE` (0x1F000000), with the
source node in the low byte. On CAN a lower ID wins arbitration, so an application that
shares the bus will always beat Muster as long as its own IDs stay below that range.
Frames outside the range are ignored. The bit rate is 500 kbit/s, and the wire format is
in [`PROTOCOL.md`](PROTOCOL.md).

## SPI

This is a point-to-point link between two ESP32-S3s. It's full duplex, sends fixed-size
packets (`[0xA5][len][payload][padding][crc16]`), and has an ATTN line the slave uses to
ask to be polled. `muster_spi_master_begin()` and `muster_spi_slave_begin()` take the pins
and the clock speed. The full design is in [`SPI_LINK_SPEC.md`](SPI_LINK_SPEC.md).

## UART

This link works over any byte stream: a hardware UART, SoftwareSerial, a USB serial
port, or a pipe in a test.

```c
static void uart_write(void *io, const uint8_t *p, uint16_t n) { ((HardwareSerial *)io)->write(p, n); }
static int  uart_read(void *io) { return ((HardwareSerial *)io)->read(); }   /* -1 when empty */

muster_uart_attach(&link, &ctx, &Serial1, uart_write, uart_read, peer_node_id, kbps);
```

Each frame goes out as `0x7E | len | payload | crc16 low | crc16 high`, with the CRC
covering the length and the payload. There's no byte stuffing. If a length or CRC is
wrong, the receiver drops that frame and looks for the next `0x7E`, so a corrupted byte
costs one frame and the runtime's retries recover it. `test_uart_link` checks exactly this
by corrupting bytes on purpose.

UART is point to point: the board at the other end is fixed (`peer`), so a lead with
three workers needs three UART links, and `MUSTER_MAX_LINKS` has to allow that. The MTU is
`MUSTER_UART_MTU` (32 by default). It has to be 253 or less and no bigger than
`MUSTER_MAX_FRAME`, and the compiler checks both. Pass the line rate divided by 1000 as
`kbps`, so 115 for 115200 baud. `recv` reads whatever bytes are waiting and returns
straight away.

## ESP-NOW

ESP-NOW is the ESP32's connectionless WiFi radio. Every board broadcasts on the same
channel, so to the runtime it looks just like a CAN bus. Every board hears every frame,
and the core ignores frames meant for someone else.

```c
static muster_link link;
static mespnow_ctx radio;

/* a worker: a plain station on the shared channel */
muster_espnow_begin(&link, &radio, node_id, /*channel=*/1, /*ap_running=*/0);

/* a lead that also serves a web page: start the access point on the same channel first */
WiFi.mode(WIFI_AP_STA);
WiFi.softAP("ssid", "password", 1);
muster_espnow_begin(&link, &radio, 0, 1, /*ap_running=*/1);
```

Each packet is the sender's node ID (one byte) followed by the Muster frame. ESP-NOW only
tells the receiver the sender's MAC address, so the node ID has to travel in the packet,
and `set_id` keeps it up to date if the node ID changes.

Broadcast frames aren't acknowledged by the radio, so some get lost. The runtime's
retries and duplicate cache make up for it, and `test_espnow_link` runs a lead and two
workers with 10% of packets lost.

The receive callback runs in the WiFi task, not in your loop. It only pushes frames into
a small ring buffer, using memory barriers instead of locks, and `muster_poll()` takes them
out, so the core stays single-threaded. If the ring is full, the frame is dropped and
counted in `radio.dropped`. `radio.sent` and `radio.send_failed` count transmissions.

You can change `MUSTER_ESPNOW_MTU` (64 by default; at most 249 and no bigger than
`MUSTER_MAX_FRAME`) and `MUSTER_ESPNOW_RING` (16 frames by default; between 2 and 255).
Both are checked when you compile.

A few things to keep in mind:

- Every board has to be on the same WiFi channel. A lead that runs an access point
  starts it on that channel, and the radio stays there. A board that also joins a WiFi
  network as a station follows that router's channel, so the other boards have to match.
- There can only be one ESP-NOW link per board, because the receive callback has no way
  to pass a context pointer.
- The radio code only compiles for Arduino-ESP32 (`ARDUINO_ARCH_ESP32`), versions 2.0.x
  and 3.x. Anywhere else, including Arduino AVR builds and the host tests, only the
  portable ring and packet code is compiled. `make -C firmware/test guards` checks that.
- Like CAN, ESP-NOW frames here are neither authenticated nor encrypted, so any radio in
  range on the same channel can join. See [`../SECURITY.md`](../SECURITY.md).

For a complete working setup, see [`firmware/examples/trio`](../firmware/examples/trio/README.md),
which has run on hardware: three ESP32s and a live dashboard.

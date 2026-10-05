# Hardware

Pins here mirror `tools/muster-cli/muster_cli/db/boards.yaml`, which is the
source of truth. Change the DB first, then this file.

## Bill of materials (prototype)
| Qty | Part | Why |
|---|---|---|
| 2 | ESP32-S3 DevKitC-1 | lead + worker; on-chip TWAI CAN, FPU, 2 cores |
| 1 | Arduino Nano (ATmega328P) | the deliberately weak worker: no CAN, no FPU, 2 KB RAM |
| 1 | MCP2515 + TJA1050 module | gives the Nano CAN over SPI |
| 2 | SN65HVD230 module | 3.3 V CAN transceiver for each ESP32 (on-chip CAN still needs a PHY) |
| 2 | 120 Ω resistor | bus termination, one at each physical end |
| 1 | 8-channel logic analyser | timing evidence for latency claims |
| — | twisted pair wire, breadboard, jumpers, powered USB hub | |

## CAN wiring
| ESP32-S3 | SN65HVD230 | | Nano | MCP2515 |
|---|---|---|---|---|
| GPIO5 (TWAI TX) | CTX | | D13 | SCK |
| GPIO4 (TWAI RX) | CRX | | D12 | SO |
| 3V3 | 3V3 | | D11 | SI |
| GND | GND | | D10 | CS |
| | | | D2 | INT |
| | | | 5V | VCC |
| | | | GND | GND |

Bus: daisy-chain CANH–CANH and CANL–CANL with **twisted pair**. Keep each
transceiver's stub to the bus short.

## Gotchas that will cost you an afternoon
1. **Termination resistors already on the modules.** Many SN65HVD230 boards have a
   120 Ω resistor fitted, and MCP2515 modules have a termination jumper (often J1).
   You need **exactly two** terminators on the whole bus, at the two physical ends.
   Remove or open the ones on any board in the middle. Measure CANH–CANL with
   power off: ~60 Ω is correct; ~40 Ω means three terminators.
2. **MCP2515 crystal.** Most cheap modules use 8 MHz; some use 16 MHz. Read the
   can on the module and set `MUSTER_MCP_CLOCK` to match, or the bit rate is wrong
   and nothing is received.
3. **Common ground.** Every board must share ground. Powering all boards from one
   USB hub does this.
4. **Logic levels.** CAN itself is safe across 3.3 V and 5 V boards because each
   transceiver isolates logic levels. A direct SPI link between a 5 V and a 3.3 V
   board needs a level shifter (and is out of scope for SPI v1 anyway).
5. **Lone node.** A CAN node with no other node to acknowledge it will retry and
   eventually go error-passive. Bring up two nodes at once.
6. **ESP32-S3 pins to avoid:** strapping pins (GPIO0, 3, 45, 46) and pins used by
   flash/PSRAM on your module variant (check your DevKit's pinout; octal-PSRAM
   variants also reserve GPIO35–37).

## SPI accelerator link (ESP32-S3 ↔ ESP32-S3, ROADMAP M5)
| Lead (master) | Worker (slave) |
|---|---|
| GPIO12 SCK | GPIO12 SCK |
| GPIO11 MOSI | GPIO11 MOSI |
| GPIO13 MISO | GPIO13 MISO |
| GPIO10 CS | GPIO10 CS |
| GPIO6 ATTN (input, pull-up) | GPIO6 ATTN (output, active low) |
| GND | GND |
These are the S3's native SPI2 (FSPI) IO-MUX pins, already in `boards.yaml`
(`spi` and `attention_pin` of `esp32-s3-devkitc-1`); confirm against your DevKit
pinout. Wires under ~15 cm; start at 4 MHz.

## Bring-up order
1. Two ESP32-S3s on CAN, no Muster: send/receive raw TWAI frames.
2. Add the Nano + MCP2515: raw frames between all three.
3. Measure the bus resistance (gotcha 1) and fix termination.
4. Only then flash the Muster examples.

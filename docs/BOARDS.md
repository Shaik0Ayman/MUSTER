# Which boards work

Muster's core is plain C99 with no dynamic memory, so in principle it builds anywhere
there's a C or C++ compiler. This page separates what we've actually checked from what
we expect.

The check is [`firmware/examples/portability`](../firmware/examples/portability). It runs a
lead and two workers inside one chip on an in-memory bus, makes some calls, unplugs a
worker, and prints `MUSTER SELFTEST: PASS` or `FAIL` over serial. It also attaches the
UART link to `Serial`, so that link gets compiled for every board too.

## Compiled locally (2026-11-07)

All 22 boards below were built with PlatformIO on one Windows machine. "Compiles" means
just that: the self-test has not been flashed to any of them yet.

| Family | Board (`pio` env) | Result | RAM used | Flash used |
|---|---|---|---|---|
| AVR | Arduino Uno (`uno`) | compiles (2 boards) | 1503 / 2048 B | 13864 / 32256 B |
| AVR | Arduino Nano (`nano`) | compiles (2 boards) | 1503 / 2048 B | 13864 / 30720 B |
| AVR | Arduino Mega 2560 (`mega2560`) | compiles | 2123 / 8192 B | 14362 / 253952 B |
| megaAVR | Arduino Nano Every (`nano_every`) | compiles | 2112 / 6144 B | 14869 / 48640 B |
| ESP32 | ESP32 DevKit (`esp32`) | compiles | 28104 / 327680 B | 276689 / 1310720 B |
| ESP32 | ESP32-S3 (`esp32_s3`) | compiles | 25072 / 327680 B | 273105 / 3342336 B |
| ESP32 | ESP32-C3 (`esp32_c3`) | compiles | 20396 / 327680 B | 257926 / 1310720 B |
| ESP32 | ESP32-S2 (`esp32_s2`) | compiles | 21200 / 327680 B | 247046 / 1310720 B |
| ESP8266 | NodeMCU v2 (`esp8266`) | compiles | 34720 / 81920 B | 275179 / 1044464 B |
| STM32 F1 | Blue Pill F103C8 (`bluepill_f103`) | compiles | 7848 / 20480 B | 17872 / 65536 B |
| STM32 F4 | Black Pill F411CE (`blackpill_f411`) | compiles | 7864 / 131072 B | 18020 / 524288 B |
| STM32 F4 | Nucleo F446RE (`nucleo_f446`) | compiles | 7900 / 131072 B | 20320 / 524288 B |
| STM32 G4 | Nucleo G431RB (`nucleo_g431`) | compiles | 8060 / 32768 B | 23368 / 131072 B |
| RP2040 | Raspberry Pi Pico (`pico`) | compiles | 47400 / 270336 B | 4070 / 2097152 B (as PlatformIO reports it) |
| nRF52 | Arduino Nano 33 BLE (`nano33ble`) | compiles | 49112 / 262144 B | 83644 / 983040 B |
| SAMD21 | Arduino Zero (`zero_samd21`) | compiles | 9632 / 32768 B | 20224 / 262144 B |
| SAMD51 | Adafruit Metro M4 (`metro_m4_samd51`) | compiles | 9308 / 196608 B | 19512 / 524288 B |
| SAM3X | Arduino Due (`due_sam3x`) | compiles | 9220 / 98304 B | 19144 / 524288 B |
| NXP i.MX RT1062 | Teensy 4.1 (`teensy41`) | compiles | 11456 B RAM1 + 12416 B RAM2 | 23164 B code + data |
| NXP Kinetis K66 | Teensy 3.6 (`teensy36`) | compiles | 9064 / 262144 B | 16828 / 1048576 B |
| NXP Kinetis KL26 | Teensy LC (`teensylc`) | compiles (2 boards) | 6192 / 8192 B | 14624 / 63488 B |
| Renesas RA4M1 | Arduino Uno R4 Minima (`uno_r4_minima`) | compiles | 9192 / 32768 B | 42248 / 262144 B |

"2 boards" means the self-test runs a lead and one worker instead of three boards, because
a whole in-memory bus plus three runtimes doesn't fit. On the Uno and Nano that's 2 KB of
RAM. On the Teensy LC (8 KB) three boards overflowed by 280 bytes. That's the self-test's
in-memory bus, not Muster itself: a real Nano worker (one runtime, no in-memory bus) uses
8854 B of flash and 492 B of RAM.

The 32-bit numbers include each core's Arduino framework, which is most of the flash.

## What has run on real hardware

Only the [`trio`](../firmware/examples/trio/README.md) setup: an ESP32-C6 lead and two
ESP32 DevKits over ESP-NOW. Joining, calls, failover and rejoin all worked. UART also
worked on hardware. The results are in [`TESTING.md`](TESTING.md#hardware-results).
Nothing else on this page has been flashed yet.

## Which links go where

| Link | Boards |
|---|---|
| UART | anything with a serial port, so every board above |
| ESP-NOW | ESP32 family only (needs Arduino-ESP32) |
| TWAI (CAN) | ESP32 and ESP32-S3 with an SN65HVD230 transceiver |
| MCP2515 (CAN) | should work on any board with SPI, but only built for the Nano so far and never run on hardware |
| SPI | ESP32-S3 pairs |

## Trying the self-test

```bash
cd firmware/examples/portability
pio run -e nano -t upload        # pick your env from platformio.ini
pio device monitor -b 115200     # look for MUSTER SELFTEST: PASS
```

The toolchain (`muster plan`) only knows two boards so far: the ESP32-S3 DevKitC-1 and the
Arduino Nano. To use any other board you can call the runtime directly. If you'd like
`muster plan` to support a board, add a datasheet-backed entry to
`tools/muster-cli/muster_cli/db/boards.yaml`.

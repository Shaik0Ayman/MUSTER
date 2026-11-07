# Examples

Hand-written PlatformIO projects that use the runtime directly. (Projects generated
from a plain C++ program by `muster-cli` are described in the top-level README.)
Each one builds with `pio run -d firmware/examples/<name>`, and CI builds them all.

| Example | Boards | Link | What it shows | Hardware status |
|---|---|---|---|---|
| [`lead_esp32`](lead_esp32/src/main.cpp) | ESP32-S3 (lead) | CAN (TWAI); `lead_esp32_spi` adds SPI to worker 1 | placing tasks, logging every event | compiles; **not run on hardware** |
| [`worker_esp32`](worker_esp32/src/main.cpp) | ESP32-S3 (worker) | CAN (TWAI); `worker_esp32_spi` adds SPI | an FPU worker holding `imu_filter` | compiles; **not run on hardware** |
| [`worker_nano`](worker_nano/src/main.cpp) | Arduino Nano (worker) | CAN (MCP2515) | the deliberately weak 8-bit worker | compiles; **not run on hardware** |
| [`sim_cluster`](sim_cluster/README.md) | one ESP32-S3 in the Wokwi simulator | in-memory 8-byte bus | a lead and two workers in one chip: failover, module moves, rejoin, with buttons and LEDs | logic host-tested (`test_sim_cluster`); simulator only |
| [`trio`](trio/README.md) | ESP32-C6 (lead) + 2 × ESP32 DevKit | ESP-NOW (no wires) | a math task balanced across two workers, PWM output, live web dashboard | **ran on hardware** 2026-11-07: join, calls, failover, rejoin, dashboard |

`lead_esp32`, `worker_esp32` and `worker_nano` form one CAN cluster: flash all
three, then wire them as in [`docs/HARDWARE.md`](../../docs/HARDWARE.md). Their shared tasks
are in [`common/demo_tasks.h`](common/demo_tasks.h).

The examples include the runtime from this repository (`lib_deps = symlink://../../`),
except `trio/muster_trio/`, which carries copies of the headers so the sketch also
opens in the Arduino IDE. `make -C firmware/test` fails if those copies drift from
`firmware/src/`; `make -C firmware/test sync-copies` refreshes them.

Transports are compared in [`docs/LINKS.md`](../../docs/LINKS.md).

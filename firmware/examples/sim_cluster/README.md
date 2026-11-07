# sim_cluster — a Muster cluster in one simulated ESP32-S3

A lead and two workers, all running the **real Muster runtime** inside one
ESP32-S3 firmware, talking over an in-memory 8-byte-frame bus that behaves like CAN.
Built to be watched in the [Wokwi](https://wokwi.com) simulator: LEDs show what each
node is doing and two buttons pull and replug the workers' cables.

## What it shows
* the lead and two workers **join**, and the workers take node IDs **hashed from a
  fake chip ID** (shared-image style);
* every second the lead calls `square(x)`; the **lead picks a worker** to run it;
* a **module** (`blink`) is assigned to exactly one board;
* **pull the cable of the board running the module**: the lead declares it lost
  after `MUSTER_NODE_TIMEOUT_MS` and **moves the module** to the other worker;
  pull both and the lead runs it itself while `square` starts failing;
* plug them back: they rejoin, and the module **stays where it is** (moving it would
  lose its state).

## What it does *not* show
It does not exercise the CAN controller, the transceivers, the wiring or real bus
timing. Wokwi simulates only **one firmware per project** and lists the ESP32-S3's CAN
controller (TWAI) as only partially implemented, so a faithful two-ESP32 CAN simulation
is not possible there. This proves the protocol logic runs on the target CPU; it
says nothing about hardware. The CAN drivers are still unverified on boards.

## Controls and LEDs
| Pin | Part | Meaning |
|---|---|---|
| GPIO4 / 5 / 6 | blue / green / yellow LED | node 0 (lead) / node 1 / node 2 |
| GPIO15 | button | pull / replug node 1's cable |
| GPIO16 | button | pull / replug node 2's cable |

LED **steady** = alive and idle, **fast blink** = running the module, **off** = unplugged.
The serial monitor (115200) prints every event and a status line every 3 s.

## Run it
1. Build the firmware: `pio run -d firmware/examples/sim_cluster`
2. Open `firmware/examples/sim_cluster` in VS Code with the
   [Wokwi extension](https://docs.wokwi.com/vscode/getting-started) (it needs a free
   license: F1 → *Wokwi: Request a Free License*), then F1 → *Wokwi: Start Simulator*.
   `wokwi.toml` already points at the PlatformIO build output.

Not yet run inside Wokwi by the authors: the logic is verified on the host
(`make -C firmware/test`, `test_sim_cluster`), the firmware builds for the ESP32-S3,
and the pin names in `diagram.json` were taken from Wokwi's own board definition.

## What you should see
This is the output of the host test running the same code, so timings in Wokwi will
differ a little:

```text
[   201] lead: node1 (id 133) joined
[   201] lead: node2 (id 132) joined
[  1501] lead: module blink -> node2 (id 132)
[  2002] square(3) = 9   ran on node1 (id 133)
[  3500] node2 cable PULLED
[  4402] lead: node2 (id 132) LOST
[  4402] lead: module blink -> node1 (id 133)
[  7800] node1 cable PULLED
[  8802] lead: node1 (id 133) LOST
[  8802] lead: module blink -> node0 (id 0)
[  8802] square(21) FAILED (-3): the board went silent mid-call
[  9001] square(24) FAILED (-2): no live board holds it
[ 10401] lead: node1 (id 133) joined
[ 11002] square(30) = 900   ran on node1 (id 133)
```

## A lesson from building it
The module is requested 1.5 s after start, once the workers have joined. Requested at
boot, the lead is the only live board, takes the module, and keeps it. A module
stays where it is, so pulling worker cables would change nothing. On real boards,
request modules after the workers have had time to join.

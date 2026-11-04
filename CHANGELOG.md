# Changelog

All notable changes are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project is pre-1.0:
the wire protocol version (`MUSTER_PROTO_VERSION`) is the compatibility contract,
and boards on different protocol versions refuse each other at `JOIN`.

## [Unreleased]

### Added
* **Lead-assigned modules** (`MUSTER_MODULE`): long-running units the lead hands to
  boards, re-places when a board is lost, and moves away from a board that declines
  to start them. Wire message `ASSIGN`, module masks in `JOIN`/`BEAT`.
* **Shared images** (`shared_images: true`): one firmware build per board *type*,
  with node IDs derived from the chip's factory MAC and ID clashes resolved by the
  lead (`ACK ok=4`).
* Modules and tasks may live in project-local headers next to the sketch; a module
  may be declared early and defined after the generated glue so it can call tasks.
* Optional `muster_link.set_id` so a CAN link learns an ID that changed after start.
* GPL-3.0-only license, README, CONTRIBUTING, SECURITY, issue and PR templates.

### Changed
* **Protocol v3** (incompatible with v2): `JOIN` 14 → 19 bytes, `ACK` gains a `uid`,
  `BEAT` gains `running`/`refused`. See `docs/PROTOCOL.md`.
* Minimal ATmega328P worker: 8120 → 8854 B flash, 468 → 492 B static RAM.
* Host tests: 101 → 130 checks. Toolchain tests: 63 → 74.

### Not yet verified
* The CAN and SPI drivers and everything built on them have **not run on real
  hardware** (roadmap M4, M5): see `docs/ROADMAP.md`.

## Earlier work (milestones M0–M9)
Protocol v2 runtime with multi-link routing, duplicate suppression and failover;
the CAN (TWAI, MCP2515) and SPI links; the libclang analyser and typed trampoline
generator; the MCP server replacing the local-model partitioner. Details and
acceptance evidence: `docs/ROADMAP.md`, `docs/DECISIONS.md`.

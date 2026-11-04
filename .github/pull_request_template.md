## What and why

## Checklist
- [ ] `make -C firmware/test` and `make -C firmware/test cpp` pass
- [ ] Core change: a host test in `firmware/test/` covers the new behaviour
- [ ] Core change: `make -C firmware/test avr` run, RAM/flash delta noted below (worker RAM < 600 B)
- [ ] Wire change: `MUSTER_PROTO_VERSION` bumped and `docs/PROTOCOL.md` updated
- [ ] Toolchain change: `pytest tools/muster-cli` passes (and a generator change was compiled with `pio run`)
- [ ] Docs updated (`docs/`, README) where behaviour changed
- [ ] Nothing claims hardware behaviour that was not observed on hardware

**Footprint delta (AVR):**

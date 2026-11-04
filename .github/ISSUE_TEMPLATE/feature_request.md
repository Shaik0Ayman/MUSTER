---
name: Feature request
about: Propose a change or a new capability
labels: enhancement
---

**What problem does this solve?**

**Proposed change.** If it touches the wire protocol, say so: protocol changes bump
`MUSTER_PROTO_VERSION` and update `docs/PROTOCOL.md`.

**Does it fit the constraints?** No dynamic allocation, no recursion, C99 core,
and it must still build for a 2 KB ATmega328P.

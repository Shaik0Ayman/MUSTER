---
name: Bug report
about: Something in the runtime or the toolchain does not behave as documented
labels: bug
---

**What happened, and what did you expect?**

**Reproduce it** (a failing host test in `firmware/test/` is ideal; otherwise the
smallest `main.cpp` + `boards.yaml` that shows it)

**Where does it happen?**
- [ ] runtime (`firmware/src/muster.h`, host test)
- [ ] on real boards (fill in the hardware report template instead)
- [ ] toolchain (`muster plan` / `build` / `deploy` / `mcp`)

**Environment:** OS, Python version, PlatformIO version, `muster` version
(`pip show muster-cli`), board types.

**Output / logs**

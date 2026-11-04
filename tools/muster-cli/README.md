# muster-cli

The toolchain for [Muster](https://github.com/Shaik0Ayman/MUSTER): split one annotated
Arduino-style program across several microcontroller boards.

```bash
pip install -e ".[test]"
muster plan examples/quadruped_modules/boards.yaml examples/quadruped_modules/main.cpp
muster build
```

`muster analyze | plan | build | deploy | boards | mcp`. `muster mcp` serves the same
operations to an AI assistant over the Model Context Protocol (stdio).

Needs [PlatformIO](https://platformio.org/) to compile for boards and, optionally,
Graphviz for the wiring diagram. Full documentation, the list of supported boards and
the limits are in the [main README](https://github.com/Shaik0Ayman/MUSTER#readme).

Licensed under GPL-3.0-only.

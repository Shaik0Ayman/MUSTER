# Security policy

## Reporting a vulnerability
Please **do not open a public issue** for a security problem. Use GitHub's private
reporting: the repository's **Security** tab → **Report a vulnerability**. Include
what you found, how to reproduce it, and the version or commit.

## What is in scope
* **The runtime parsing hostile input.** Any frame or message that makes
  `muster_poll()` read or write out of bounds, hang, or corrupt state is a bug.
  Malformed and oversized input is already covered by `firmware/test/test_framing.c`
  and `test_core.c`; a new case is welcome as a failing test.
* **The toolchain.** Path handling, command construction for `pio`, and the MCP
  server (`muster mcp`). `deploy` over MCP is a dry run unless `confirm=true`;
  anything that bypasses that, or that lets a plan skip `validate()`, is in scope.

## What is by design
* **The bus is not authenticated or encrypted.** Any node on the CAN bus (or SPI
  link) can join, impersonate a node ID, or send calls. Muster assumes a bus you
  physically control. Do not rely on it for safety or security-critical functions
  without adding your own protection. This is documented, not a vulnerability.
* The MCP server trusts the MCP client that launched it and runs with your user's
  permissions.

## Supported versions
Only the latest commit on `main` is supported while the project is pre-1.0.

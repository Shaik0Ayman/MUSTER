# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
"""Embed the dashboard page (index.html, next to this file) into
firmware/src/muster_dashboard_page.h, for boards that host the page themselves.

    python3 tools/muster-cli/muster_cli/dash/embed.py           # regenerate
    python3 tools/muster-cli/muster_cli/dash/embed.py --check   # fail if out of date

Standalone (stdlib only), so `make -C firmware/test` can run the check.
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAGE = HERE / "index.html"
OUT = HERE.parents[3] / "firmware" / "src" / "muster_dashboard_page.h"
DELIM = "MUSTERDASH"


def render():
    html = PAGE.read_text(encoding="utf-8")
    if ")" + DELIM + '"' in html:
        raise SystemExit("index.html contains the raw-string delimiter")
    return ("/* SPDX-License-Identifier: GPL-3.0-only */\n"
            "/* GENERATED from tools/muster-cli/muster_cli/dash/index.html by dash/embed.py.\n"
            " * Do not edit: change index.html and re-run embed.py. */\n"
            "#ifndef MUSTER_DASHBOARD_PAGE_H\n#define MUSTER_DASHBOARD_PAGE_H\n"
            f"/* {len(html.encode('utf-8'))} bytes, kept in flash */\n"
            f'static const char MUSTER_DASHBOARD_PAGE[] = R"{DELIM}(' + html + f'){DELIM}";\n'
            "#endif\n")


def main():
    want = render()
    if "--check" in sys.argv:
        have = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if have != want:
            raise SystemExit(f"{OUT.name} is out of date: run python3 {Path(__file__).as_posix()}")
        print("dashboard page header matches index.html")
        return
    OUT.write_text(want, encoding="utf-8", newline="\n")
    print(f"wrote {OUT} ({len(want)} bytes)")


if __name__ == "__main__":
    main()

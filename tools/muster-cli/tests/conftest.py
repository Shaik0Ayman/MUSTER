# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 The Muster contributors
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "examples" / "quadruped"
BOARDS = EXAMPLE / "boards.yaml"
SOURCE = EXAMPLE / "main.cpp"
FIRMWARE_SRC = ROOT.parents[1] / "firmware" / "src"

sys.path.insert(0, str(ROOT))


@pytest.fixture
def sketch(tmp_path):
    """Write a sketch into tmp_path and return its path."""
    def write(body, name="main.cpp"):
        p = tmp_path / name
        p.write_text("#include <Arduino.h>\n" + body, encoding="utf-8")
        return p
    return write


@pytest.fixture
def analyse_text(sketch):
    from muster_cli.analyze import analyze_file
    return lambda body: analyze_file(sketch(body))


def _wsl_path(p):
    p = Path(p).resolve()
    return "/mnt/" + p.drive[0].lower() + p.as_posix()[2:]


def host_cxx():
    """A host toolchain: run(cwd, argv) -> CompletedProcess, where argv[0] is
    "c++" for the compiler or "./prog" for a program it built. None if absent.

    Uses $MUSTER_HOST_CXX, else g++/clang++ on PATH. On Windows without one,
    MUSTER_HOST_CXX=wsl (or wsl:<distro>) compiles and runs inside WSL."""
    want = os.environ.get("MUSTER_HOST_CXX", "")
    if want.startswith("wsl"):
        distro = want.split(":", 1)[1] if ":" in want else "Ubuntu-24.04"

        def run(cwd, argv):
            argv = ["g++" if a == "c++" else a for a in argv]
            conv = [_wsl_path(a) if Path(a).is_absolute() else a for a in argv]
            cmd = "cd '" + _wsl_path(cwd) + "' && " + " ".join(f"'{c}'" for c in conv)
            return subprocess.run(["wsl.exe", "-d", distro, "-e", "bash", "-c", cmd],
                                  capture_output=True, text=True)
        return run
    exe = want or shutil.which("g++") or shutil.which("clang++")
    if not exe:
        return None
    return lambda cwd, argv: subprocess.run([exe if a == "c++" else a for a in argv],
                                            cwd=cwd, capture_output=True, text=True)

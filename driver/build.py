#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Build the WSF guest drivers and regenerate their headers with ntdlib's build_ppc_driver.py: driver.c (NW4R SND) as
src/rip/driver_blob.h, and jaudio.c (JAudio2) as src/rip/jaudio_driver_blob.h.

Requires a clang with the PowerPC target. LLVM's releases and Homebrew's llvm have that target. Apple's clang doesn't.
Run the script only when a driver changes. The generated files are committed. Building WSF itself requires no PowerPC
toolchain.

usage: driver/build.py [--clang PATH]
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    # The paths are relative to the repository's root. The generated header names the source as given.
    drivers = [("driver/driver.c", "src/rip/driver_blob.h", "wsf::rip"),
               ("driver/jaudio.c", "src/rip/jaudio_driver_blob.h", "wsf::rip::jaudio")]
    for source, output, namespace in drivers:
        command = [sys.executable, "externals/ntdlib/tools/build_ppc_driver.py", "--source", source, "--output", output,
                   "--namespace", namespace, "--product", "WSF", "--command", "driver/build.py", *sys.argv[1:]]
        result = subprocess.run(command, cwd=ROOT).returncode
        if result != 0:
            return result

    return 0


if __name__ == "__main__":
    sys.exit(main())

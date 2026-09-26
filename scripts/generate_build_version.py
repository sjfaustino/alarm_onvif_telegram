import os
import subprocess
from datetime import datetime

# PlatformIO "pre:" script: writes include/generated_build_version.h with
# FIRMWARE_BUILD_VERSION, taken from `git describe` so every build names the
# commit it came from:
#   v1.2.0                  -> "1.2.0"               (a tagged release)
#   v1.2.0-5-gabc1234       -> "1.2.0-5-gabc1234"    (5 commits after it)
#   ...-dirty               uncommitted changes in the working tree
# Without git or tags (e.g. a source tarball) it falls back to
# "dev-YYYYMMDD.HHMM".
#
# Only build_version.cpp includes the generated header, and the file is
# rewritten only when the value changes, so an unchanged tree rebuilds
# nothing.

HEADER = "include/generated_build_version.h"


def git_version():
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--dirty", "--always", "--match", "v[0-9]*"],
            capture_output=True, text=True, check=True, timeout=10,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    if not out:
        return None
    return out[1:] if out.startswith("v") else out


version = git_version() or "dev-" + datetime.now().strftime("%Y%m%d.%H%M")
content = (
    "#pragma once\n"
    "// GENERATED FILE - see scripts/generate_build_version.py. Do not edit by hand.\n"
    '#define FIRMWARE_BUILD_VERSION "{}"\n'.format(version)
)

try:
    with open(HEADER) as f:
        unchanged = f.read() == content
except OSError:
    unchanged = False

if not unchanged:
    with open(HEADER, "w") as f:
        f.write(content)
print("[generate_build_version] FIRMWARE_BUILD_VERSION = {}".format(version))

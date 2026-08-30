from datetime import datetime

# Runs on every `pio run`/upload (a PlatformIO "pre:" extra_script, invoked
# before any compilation starts, not just once ever) - computes the current
# local wall-clock time as "YYYYMMDD.HHMM" and (re)writes
# include/generated_build_version.h with it, defining FIRMWARE_BUILD_VERSION
# (see include/build_version.h/build_version.cpp for how that's consumed).
# Year-first so two versions sort correctly as plain strings/numbers - the
# original DDMMYYYY order didn't (e.g. "29082026" > "01092026" even though
# September 1st is the later build).
#
# Written as its OWN tiny generated header - deliberately NOT injected as a
# CPPDEFINES build flag applied to the whole build. An earlier version of
# this script did exactly that, and it was a real, measured problem: the
# define reached every file that (transitively) included config.h, i.e.
# nearly all of src/, so SCons saw every one of those files' compile
# command line as "changed" on every single invocation (this value changes
# every minute) and did a full rebuild every time - including a `pio run`
# with zero source edits, and every `-t upload`. Measured ~6-7x slower
# (~2m20s full rebuild vs. ~20s real incremental no-op) as a permanent,
# ongoing tax, not a one-time cost. Writing a small generated header that
# only ONE translation unit (build_version.cpp) includes means SCons only
# ever needs to recompile that one file (plus relink) when the timestamp
# changes - everything else stays cached, same as a normal incremental
# build.
build_version = datetime.now().strftime("%Y%m%d.%H%M")
with open("include/generated_build_version.h", "w") as f:
    f.write("#pragma once\n")
    f.write("// GENERATED FILE - see scripts/generate_build_version.py. Do not edit by hand.\n")
    f.write('#define FIRMWARE_BUILD_VERSION "{}"\n'.format(build_version))
print("[generate_build_version] FIRMWARE_BUILD_VERSION = {}".format(build_version))

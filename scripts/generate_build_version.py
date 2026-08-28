Import("env")

from datetime import datetime

# Runs on every `pio run`/upload (a PlatformIO "pre:" extra_script, invoked
# before any compilation starts, not just once ever) - computes the current
# local wall-clock time as "DDMMYYYY.HHMM" and injects it as a preprocessor
# define, FIRMWARE_BUILD_VERSION (see include/config.h's own comment for how
# it's consumed). Passed via CPPDEFINES as an (name, value) tuple - not a
# plain "-D..." string in build_flags - specifically because SCons handles
# the quoting for tuple-form CPPDEFINES correctly on both Windows and Linux;
# a hand-escaped "-DFOO=\"...\"" string in build_flags is a classic source
# of "works on my platform, broken on the other one" quoting bugs.
#
# This is a real (not just cosmetic) reason config.h can't just compute this
# from __DATE__/__TIME__ directly instead: those macros reflect when that
# ONE .cpp file was compiled, and PlatformIO only recompiles files that
# actually changed - editing one file and rebuilding would leave every
# *other* file's __DATE__/__TIME__ stamped with an old build's timestamp,
# so different dashboard pages could show different "versions" from the same
# firmware image. Because config.h is included by nearly every file in
# src/, changing this single build-wide define also means SCons sees every
# one of those files' compile command line as changed and rebuilds them -
# so every `pio run` ends up close to a full rebuild. That's the accepted
# cost of a version that's genuinely accurate on every single build, not an
# oversight.
build_version = datetime.now().strftime("%d%m%Y.%H%M")
env.Append(CPPDEFINES=[("FIRMWARE_BUILD_VERSION", '\\"{}\\"'.format(build_version))])
print("[generate_build_version] FIRMWARE_BUILD_VERSION = {}".format(build_version))

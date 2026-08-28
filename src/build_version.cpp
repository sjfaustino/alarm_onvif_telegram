#include "build_version.h"
#include "generated_build_version.h" // rewritten fresh before every build - see scripts/generate_build_version.py

// #ifdef, not a hard dependency on the generated header actually defining
// this - generated_build_version.h is gitignored and untracked (a real
// `pio run -e esp32s3` always creates/rewrites it before compiling
// anything, via the pre: extra_script, so this isn't needed in practice),
// but stays defensive in case some build path ever skips that script.
#ifdef FIRMWARE_BUILD_VERSION
const char* FIRMWARE_VERSION = FIRMWARE_BUILD_VERSION;
#else
const char* FIRMWARE_VERSION = "dev";
#endif

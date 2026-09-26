#include "build_version.h"
#include "generated_build_version.h" // rewritten fresh before every build - see scripts/generate_build_version.py

// The pre: script always generates the header; "dev" only covers a build path
// that skips it.
#ifdef FIRMWARE_BUILD_VERSION
const char* FIRMWARE_VERSION = FIRMWARE_BUILD_VERSION;
#else
const char* FIRMWARE_VERSION = "dev";
#endif

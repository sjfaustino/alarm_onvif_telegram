#include "build_version.h"
#include "generated_build_version.h" // rewritten fresh before every build - see scripts/generate_build_version.py

// #ifdef, not a hard dependency on the generated header actually defining
// this - generated_build_version.h itself always exists (a placeholder
// ships in the repo, see that file's own comment), but stays defensive
// in case some build path ever skips the generating script.
#ifdef FIRMWARE_BUILD_VERSION
const char* FIRMWARE_VERSION = FIRMWARE_BUILD_VERSION;
#else
const char* FIRMWARE_VERSION = "dev";
#endif

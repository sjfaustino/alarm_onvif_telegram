#pragma once

// Build stamp "YYYYMMDD.HHMM" (scripts/generate_build_version.py). Kept in its
// own translation unit so a value that changes every build doesn't force a
// full rebuild (~6-7x slower).
extern const char* FIRMWARE_VERSION;

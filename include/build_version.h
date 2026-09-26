#pragma once

// Firmware version from `git describe` - e.g. "1.2.0" for a tagged release,
// "1.2.0-5-gabc1234[-dirty]" after it (scripts/generate_build_version.py).
// Kept in its own translation unit so a version change recompiles one file.
extern const char* FIRMWARE_VERSION;

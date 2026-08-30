#pragma once

// The running firmware's build-time version stamp, "YYYYMMDD.HHMM" (year-
// first so two versions sort correctly) - see
// scripts/generate_build_version.py for how it's computed.
//
// Its own translation unit (build_version.cpp), not a config.h constant:
// config.h is included by nearly every src/ file, and this value changes
// on every build - baking it in there would touch every file's compile
// command line each time, forcing a full rebuild instead of an
// incremental one (measured ~6-7x slower). Isolating it here means only
// build_version.cpp recompiles when the timestamp changes.
extern const char* FIRMWARE_VERSION;

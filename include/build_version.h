#pragma once

// The running firmware's build-time version stamp, "DDMMYYYY.HHMM" - see
// scripts/generate_build_version.py for how it's computed fresh before
// every build.
//
// Deliberately its own tiny translation unit (build_version.cpp), NOT a
// constant in config.h, even though config.h is where every other
// project-wide constant lives: config.h is #included by nearly every
// file in src/, and this value changes on every single `pio run`/upload
// (by design - see the script's own comment) - baking it into config.h
// would make every one of those files' compile command line look
// "changed" every time, forcing SCons to rebuild the entire project on
// every invocation, including ones with zero source edits (measured:
// ~6-7x slower than a real incremental no-op build). Isolating it here
// means only build_version.cpp itself needs to recompile when the
// timestamp changes - everything else stays cached.
extern const char* FIRMWARE_VERSION;

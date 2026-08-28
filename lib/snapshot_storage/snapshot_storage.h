#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <vector>

// Pure decision logic for SD-backed snapshot history (sd_store.h/.cpp owns
// the actual filesystem I/O) - split out here so it's unit-testable
// natively (test/test_snapshot_storage), same split this project already
// uses for every other bit of logic worth getting right without needing
// real hardware to check it (see test/README.md).

// Deterministic camera-name -> filesystem directory name mapping. NOT a
// lossy strip-only sanitizer like webserver_network.cpp's
// sanitizeHostname() - that's fine for a single cosmetic value (the
// mDNS hostname), but here two *different* camera names must never
// collide on the same directory: two live cameras writing/pruning into
// what they each believe is their own directory would corrupt each
// other's history. Strips to alnum+hyphen, then appends "-<4 hex
// digits>" of an FNV-1a hash of the ORIGINAL (unsanitized, case-
// preserved) name - so the same camera always maps to the same
// directory across reboots with no persisted mapping needed, and two
// differently-named cameras essentially never collide, including the
// case that actually matters on a FAT filesystem (case-insensitive
// directory names): since the hash is computed over the case-sensitive
// original string, "Camera1" and "camera1" hash to different values,
// so their full directory names differ in more than just letter case.
String sanitizeCameraDirName(const String& cameraName);

// One file already on SD, for pruning decisions - filename only (no
// path), since filesToPrune operates within a single camera's own
// directory.
struct SnapshotFileInfo {
  String name;
  uint64_t size = 0;
};

// Given a camera's own files (OLDEST FIRST - the caller sorts, since
// filenames are timestamp-prefixed and sort chronologically) and how many
// bytes need to be reclaimed, returns which filenames to delete, oldest
// first, to free at least bytesNeeded - capped at maxFiles regardless of
// whether that's actually enough. The cap is deliberate, not just a nice-
// to-have: sd_store.cpp holds the SD mutex for the whole prune-then-write
// pass, so an unbounded "keep deleting until reclaimed or empty" loop
// would let a single write hold that mutex (and block every other
// camera's own writes) for an unpredictable, potentially long time on a
// nearly-full or corrupted card. If one call's cap isn't enough, the next
// write's own prune pass continues the job.
std::vector<String> filesToPrune(const std::vector<SnapshotFileInfo>& filesOldestFirst,
                                  uint64_t bytesNeeded, size_t maxFiles);

#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <vector>

// Pure decision logic for SD snapshot history (sd_store.cpp does the I/O),
// tested natively.

// Camera name -> directory name: alnum/hyphen plus a 4-hex FNV-1a hash of the
// original, case-sensitive name. Stable across reboots with no stored mapping,
// and distinct names don't collide even on case-insensitive FAT ("Camera1" vs
// "camera1") - a collision would let two cameras prune each other's history.
String sanitizeCameraDirName(const String& cameraName);

// A file on SD. path is the full path (files span Year/Month/Day folders); the
// decision functions use only name/size.
struct SnapshotFileInfo {
  String name;
  uint64_t size = 0;
  String path;
};

// Oldest-first files to delete to free bytesNeeded, capped at maxFiles even if
// that's not enough: the SD mutex is held while pruning, so this bounds how
// long other cameras' writes wait. The next write continues.
std::vector<String> filesToPrune(const std::vector<SnapshotFileInfo>& filesOldestFirst,
                                  uint64_t bytesNeeded, size_t maxFiles);

// Epoch time from the "YYYYMMDD-HHMMSS" filename prefix (local time), or -1.
// Capture time lives in the name because SD.h mtime is unreliable.
time_t parseSnapshotTimestamp(const String& filename);

// Files strictly older than retentionDays at nowEpoch; 0 = keep forever.
// Unparseable names are left alone.
std::vector<String> filesToExpire(const std::vector<SnapshotFileInfo>& filesOldestFirst,
                                   uint16_t retentionDays, time_t nowEpoch);

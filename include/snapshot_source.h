#pragma once
#include <Arduino.h>

// What triggered a stored snapshot, tagged on every entry (RAM ring and SD) so
// the Gallery and Preview can show why it was taken. Motion is the generic
// case and the fallback for old SD files without a tag.
enum class SnapshotSource : uint8_t {
  Motion,
  Person,
  Vehicle,
  Pet,
  Tamper,
  Timelapse,
  Test,
  Manual,
};

// Lowercase English label; also the SD filename suffix.
const char* snapshotSourceLabel(SnapshotSource source);

// Motion for unknown or missing labels.
SnapshotSource snapshotSourceFromLabel(const String& label);

// Source plus capture day ("YYYYMMDD", local) for Gallery date browsing. date
// is SD-only (from the filename); the RAM ring reports "".
struct SnapshotEntryInfo {
  SnapshotSource source = SnapshotSource::Motion;
  String date;
};

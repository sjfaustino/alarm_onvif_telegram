#pragma once
#include <Arduino.h>

// What triggered a stored snapshot - CameraState::snapshotHistory's PSRAM
// ring (camera.h) and sd_store.h's SD-backed history both tag every entry
// with one of these, so the Gallery page (webserver_gallery.cpp) and the
// Cameras page's Preview column (webserver_cameras.cpp) can show "why was
// this photo taken" without cross-referencing the Activity log by
// timestamp. A superset of telegram_i18n.h's MotionDetectionKind (which
// only distinguishes Generic/Person/Vehicle for the Telegram caption
// itself) - Pet/Tamper/Timelapse/Test/Manual each come from a completely
// different trigger path in telegram.cpp, not a motion classification.
//
// Motion is both the generic/no-classification case (plain PIR/cell
// motion, or an ONVIF feed that doesn't distinguish person/vehicle) and
// the safe fallback for an SD-backed snapshot captured before this
// feature existed - its filename has no source suffix to parse (see
// sd_store.cpp's parseSnapshotSourceFromFilename).
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

// Short, lowercase, English-only label - same "filename/log-facing, not
// recipient-facing, so no i18n" reasoning as telegram.cpp's own
// motionKindLogLabel. Doubles as the SD filename suffix (sd_store.cpp)
// and the Gallery/Preview column's display text.
const char* snapshotSourceLabel(SnapshotSource source);

// Reverse of snapshotSourceLabel, for parsing a source back out of an SD
// filename's suffix. Returns SnapshotSource::Motion for any unrecognized
// or missing label - indistinguishable from (and just as correct a
// fallback for) a plain generic-motion snapshot.
SnapshotSource snapshotSourceFromLabel(const String& label);

// One stored snapshot's source tag plus which calendar day it was
// captured on ("YYYYMMDD", local time, matching sd_store.cpp's
// buildSnapshotFilename) - the Gallery page's date-range browsing
// (webserver_gallery.cpp) needs both per entry. date is only ever
// populated for SD-backed history, parsed from the filename (which
// encodes it regardless of which Year/Month/Day folder, or the legacy
// flat layout, it's actually stored under) - the PSRAM ring only keeps a
// boot-relative millis() timestamp, not a wall-clock date, so every entry
// there reports "" (see snapshot_history.cpp's ramSnapshotEntriesAll).
struct SnapshotEntryInfo {
  SnapshotSource source = SnapshotSource::Motion;
  String date;
};

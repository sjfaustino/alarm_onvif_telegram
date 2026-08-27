#include "camera_serialize.h"

// Non-printable ASCII separator between a single camera's fields - no
// camera name, URL, credential, or note should ever legitimately contain
// this, so records are just split/joined on it rather than building full
// field-escaping machinery. Not the same separator camera_store.cpp uses
// *between* records (its own RECORD_SEP, private to that file) - this one
// only needs to be consistent within a single serialize/deserialize pair.
static const char FIELD_SEP = '\x1F';

static String stripSeparators(const String& s) {
  String out = s;
  out.replace(String(FIELD_SEP), "");
  return out;
}

static std::vector<String> splitFields(const String& record) {
  std::vector<String> fields;
  int fieldStart = 0;
  for (int i = 0; i <= (int)record.length(); i++) {
    if (i == (int)record.length() || record[i] == FIELD_SEP) {
      fields.push_back(record.substring(fieldStart, i));
      fieldStart = i + 1;
    }
  }
  return fields;
}

// Always emits CAMERA_SCHEMA_VERSION's layout - this function has exactly
// one field order, ever. A layout change means bumping the version and
// writing a new deserializeCamera branch for it, not editing this one in
// place.
String serializeCamera(const CameraConfig& c) {
  String s;
  s += stripSeparators(c.name);                       s += FIELD_SEP;
  s += stripSeparators(c.deviceServiceUrl);            s += FIELD_SEP;
  s += (c.enabled ? "1" : "0");                        s += FIELD_SEP;
  s += (c.useWSSecurity ? "1" : "0");                  s += FIELD_SEP;
  s += (c.includeInitialTerminationTime ? "1" : "0");  s += FIELD_SEP;
  s += (c.includeReplyToAnonymous ? "1" : "0");        s += FIELD_SEP;
  s += stripSeparators(c.snapshotUriOverride);         s += FIELD_SEP;
  s += stripSeparators(c.preferredProfileKeyword);     s += FIELD_SEP;
  s += stripSeparators(c.user);                        s += FIELD_SEP;
  s += stripSeparators(c.pass);                        s += FIELD_SEP;
  s += stripSeparators(c.notes);                       s += FIELD_SEP;
  s += String(c.alertCooldownMs);                      s += FIELD_SEP;
  s += String(c.offlineThresholdMs);                   s += FIELD_SEP;
  s += String(c.snapshotBurstCount);
  return s;
}

// Version 0: pre-versioning format, shipped for a while before
// CAMERA_SCHEMA_VERSION existed. Tolerant of the last three fields being
// absent (11, 12, or 13 fields, in addition to the full 14) because
// alertCooldownMs/offlineThresholdMs/snapshotBurstCount were, as a matter
// of historical fact, only ever appended in that order - never inserted
// or reordered. That's exactly the assumption a real version number now
// exists so nothing has to keep relying on it again after this.
static CameraConfig deserializeCameraV0(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() < 11) return c; // malformed - caller skips entries with an empty name

  c.name                          = fields[0];
  c.deviceServiceUrl              = fields[1];
  c.enabled                       = fields[2] == "1";
  c.useWSSecurity                 = fields[3] == "1";
  c.includeInitialTerminationTime = fields[4] == "1";
  c.includeReplyToAnonymous       = fields[5] == "1";
  c.snapshotUriOverride           = fields[6];
  c.preferredProfileKeyword       = fields[7];
  c.user                          = fields[8];
  c.pass                          = fields[9];
  c.notes                         = fields[10];
  if (fields.size() >= 12 && fields[11].length() > 0) {
    c.alertCooldownMs = (unsigned long)fields[11].toInt();
  }
  if (fields.size() >= 13 && fields[12].length() > 0) {
    c.offlineThresholdMs = (unsigned long)fields[12].toInt();
  }
  if (fields.size() >= 14 && fields[13].length() > 0) {
    c.snapshotBurstCount = (unsigned int)fields[13].toInt();
  }
  return c;
}

// Version 1 (CAMERA_SCHEMA_VERSION): the current, fixed 14-field layout.
// Unlike V0, this requires an *exact* field count - a record tagged as
// this version with the wrong number of fields is corruption (or a
// firmware downgrade after a newer version changed the layout), not
// something to guess at, so it's rejected (empty name) rather than
// partially/incorrectly populated.
static CameraConfig deserializeCameraV1(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() != 14) return c; // malformed - caller skips entries with an empty name

  c.name                          = fields[0];
  c.deviceServiceUrl              = fields[1];
  c.enabled                       = fields[2] == "1";
  c.useWSSecurity                 = fields[3] == "1";
  c.includeInitialTerminationTime = fields[4] == "1";
  c.includeReplyToAnonymous       = fields[5] == "1";
  c.snapshotUriOverride           = fields[6];
  c.preferredProfileKeyword       = fields[7];
  c.user                          = fields[8];
  c.pass                          = fields[9];
  c.notes                         = fields[10];
  if (fields[11].length() > 0) c.alertCooldownMs   = (unsigned long)fields[11].toInt();
  if (fields[12].length() > 0) c.offlineThresholdMs = (unsigned long)fields[12].toInt();
  if (fields[13].length() > 0) c.snapshotBurstCount = (unsigned int)fields[13].toInt();
  return c;
}

CameraConfig deserializeCamera(const String& record, uint16_t recordVersion) {
  std::vector<String> fields = splitFields(record);

  if (recordVersion == 0) return deserializeCameraV0(fields);
  if (recordVersion == CAMERA_SCHEMA_VERSION) return deserializeCameraV1(fields);

  // Unknown version, newer than anything this firmware knows about (most
  // likely: downgraded after a later firmware version changed the layout).
  // Best-effort fall through to the newest known layout instead of
  // discarding the record outright - camera_store.cpp logs a clear
  // warning when this happens so it doesn't go unnoticed.
  return deserializeCameraV1(fields);
}

size_t cameraRecordFieldCount(const String& record) {
  return splitFields(record).size();
}

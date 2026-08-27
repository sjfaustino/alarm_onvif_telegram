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

CameraConfig deserializeCamera(const String& record) {
  CameraConfig c;
  std::vector<String> fields;
  int fieldStart = 0;
  for (int i = 0; i <= (int)record.length(); i++) {
    if (i == (int)record.length() || record[i] == FIELD_SEP) {
      fields.push_back(record.substring(fieldStart, i));
      fieldStart = i + 1;
    }
  }
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
  // alertCooldownMs/offlineThresholdMs/snapshotBurstCount were added after
  // the original 11-field format - records saved before each of these
  // existed keep CameraConfig's defaults via fields.size().
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

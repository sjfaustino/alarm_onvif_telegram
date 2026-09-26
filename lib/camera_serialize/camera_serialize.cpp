#include "camera_serialize.h"
#include <algorithm>

// Field separator within one record (ASCII unit separator - never in real
// data, so no escaping). Records are joined by camera_store's RECORD_SEP.
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

// One layout, ever. Changing it means a new version and deserialize branch.
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
  s += String(c.snapshotBurstCount);                   s += FIELD_SEP;
  s += (c.quietHoursEnabled ? "1" : "0");              s += FIELD_SEP;
  s += String(c.quietStartMinute);                     s += FIELD_SEP;
  s += String(c.quietEndMinute);                       s += FIELD_SEP;
  s += String(c.motionWatchdogHours);                  s += FIELD_SEP;
  s += String(c.timelapseIntervalMin);                 s += FIELD_SEP;
  s += String(c.pollIntervalMs);                       s += FIELD_SEP;
  s += (c.timelapseSendToTelegram ? "1" : "0");        s += FIELD_SEP;
  s += String(c.retentionDays);                        s += FIELD_SEP;
  s += (c.petAlertsEnabled ? "1" : "0");                s += FIELD_SEP;
  s += (c.petAlertsTextOnly ? "1" : "0");               s += FIELD_SEP;
  s += String(c.snapshotMaxWidth);                      s += FIELD_SEP;
  s += String(c.snapshotMaxHeight);                     s += FIELD_SEP;
  s += (c.personAlertsEnabled ? "1" : "0");             s += FIELD_SEP;
  s += (c.vehicleAlertsEnabled ? "1" : "0");             s += FIELD_SEP;
  s += (c.motionDigestEnabled ? "1" : "0");
  return s;
}

// V0 (before versioning): 11-14 fields, since the last three were only ever
// appended.
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

// Each version below has its own branch, never edited, so older records keep
// parsing; loadCameras re-saves in the current version. From V1 on the field
// count must match exactly.
//
// V1: 14 fields.
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

// V2: + quiet hours (3), motion watchdog, timelapse = 19.
static CameraConfig deserializeCameraV2(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() != 19) return c; // malformed - caller skips entries with an empty name

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
  if (fields[11].length() > 0) c.alertCooldownMs    = (unsigned long)fields[11].toInt();
  if (fields[12].length() > 0) c.offlineThresholdMs  = (unsigned long)fields[12].toInt();
  if (fields[13].length() > 0) c.snapshotBurstCount  = (unsigned int)fields[13].toInt();
  c.quietHoursEnabled              = fields[14] == "1";
  if (fields[15].length() > 0) c.quietStartMinute    = (uint16_t)fields[15].toInt();
  if (fields[16].length() > 0) c.quietEndMinute      = (uint16_t)fields[16].toInt();
  if (fields[17].length() > 0) c.motionWatchdogHours = (uint16_t)fields[17].toInt();
  if (fields[18].length() > 0) c.timelapseIntervalMin = (uint16_t)fields[18].toInt();
  return c;
}

// V3: + pollIntervalMs = 20.
static CameraConfig deserializeCameraV3(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() != 20) return c; // malformed - caller skips entries with an empty name

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
  if (fields[11].length() > 0) c.alertCooldownMs    = (unsigned long)fields[11].toInt();
  if (fields[12].length() > 0) c.offlineThresholdMs  = (unsigned long)fields[12].toInt();
  if (fields[13].length() > 0) c.snapshotBurstCount  = (unsigned int)fields[13].toInt();
  c.quietHoursEnabled              = fields[14] == "1";
  if (fields[15].length() > 0) c.quietStartMinute    = (uint16_t)fields[15].toInt();
  if (fields[16].length() > 0) c.quietEndMinute      = (uint16_t)fields[16].toInt();
  if (fields[17].length() > 0) c.motionWatchdogHours = (uint16_t)fields[17].toInt();
  if (fields[18].length() > 0) c.timelapseIntervalMin = (uint16_t)fields[18].toInt();
  if (fields[19].length() > 0) c.pollIntervalMs      = (unsigned long)fields[19].toInt();
  return c;
}

// V4: + timelapseSendToTelegram, retentionDays = 22.
static CameraConfig deserializeCameraV4(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() != 22) return c; // malformed - caller skips entries with an empty name

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
  if (fields[11].length() > 0) c.alertCooldownMs    = (unsigned long)fields[11].toInt();
  if (fields[12].length() > 0) c.offlineThresholdMs  = (unsigned long)fields[12].toInt();
  if (fields[13].length() > 0) c.snapshotBurstCount  = (unsigned int)fields[13].toInt();
  c.quietHoursEnabled              = fields[14] == "1";
  if (fields[15].length() > 0) c.quietStartMinute    = (uint16_t)fields[15].toInt();
  if (fields[16].length() > 0) c.quietEndMinute      = (uint16_t)fields[16].toInt();
  if (fields[17].length() > 0) c.motionWatchdogHours = (uint16_t)fields[17].toInt();
  if (fields[18].length() > 0) c.timelapseIntervalMin = (uint16_t)fields[18].toInt();
  if (fields[19].length() > 0) c.pollIntervalMs      = (unsigned long)fields[19].toInt();
  c.timelapseSendToTelegram        = fields[20] == "1";
  if (fields[21].length() > 0) c.retentionDays       = (uint16_t)fields[21].toInt();
  return c;
}

// V5: + petAlertsEnabled, petAlertsTextOnly = 24.
static CameraConfig deserializeCameraV5(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() != 24) return c; // malformed - caller skips entries with an empty name

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
  if (fields[11].length() > 0) c.alertCooldownMs    = (unsigned long)fields[11].toInt();
  if (fields[12].length() > 0) c.offlineThresholdMs  = (unsigned long)fields[12].toInt();
  if (fields[13].length() > 0) c.snapshotBurstCount  = (unsigned int)fields[13].toInt();
  c.quietHoursEnabled              = fields[14] == "1";
  if (fields[15].length() > 0) c.quietStartMinute    = (uint16_t)fields[15].toInt();
  if (fields[16].length() > 0) c.quietEndMinute      = (uint16_t)fields[16].toInt();
  if (fields[17].length() > 0) c.motionWatchdogHours = (uint16_t)fields[17].toInt();
  if (fields[18].length() > 0) c.timelapseIntervalMin = (uint16_t)fields[18].toInt();
  if (fields[19].length() > 0) c.pollIntervalMs      = (unsigned long)fields[19].toInt();
  c.timelapseSendToTelegram        = fields[20] == "1";
  if (fields[21].length() > 0) c.retentionDays       = (uint16_t)fields[21].toInt();
  c.petAlertsEnabled                = fields[22] == "1";
  c.petAlertsTextOnly               = fields[23] == "1";
  return c;
}

// V6: + snapshotMaxWidth/Height = 26.
static CameraConfig deserializeCameraV6(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() != 26) return c; // malformed - caller skips entries with an empty name

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
  if (fields[11].length() > 0) c.alertCooldownMs    = (unsigned long)fields[11].toInt();
  if (fields[12].length() > 0) c.offlineThresholdMs  = (unsigned long)fields[12].toInt();
  if (fields[13].length() > 0) c.snapshotBurstCount  = (unsigned int)fields[13].toInt();
  c.quietHoursEnabled              = fields[14] == "1";
  if (fields[15].length() > 0) c.quietStartMinute    = (uint16_t)fields[15].toInt();
  if (fields[16].length() > 0) c.quietEndMinute      = (uint16_t)fields[16].toInt();
  if (fields[17].length() > 0) c.motionWatchdogHours = (uint16_t)fields[17].toInt();
  if (fields[18].length() > 0) c.timelapseIntervalMin = (uint16_t)fields[18].toInt();
  if (fields[19].length() > 0) c.pollIntervalMs      = (unsigned long)fields[19].toInt();
  c.timelapseSendToTelegram        = fields[20] == "1";
  if (fields[21].length() > 0) c.retentionDays       = (uint16_t)fields[21].toInt();
  c.petAlertsEnabled                = fields[22] == "1";
  c.petAlertsTextOnly               = fields[23] == "1";
  if (fields[24].length() > 0) c.snapshotMaxWidth    = (uint16_t)fields[24].toInt();
  if (fields[25].length() > 0) c.snapshotMaxHeight   = (uint16_t)fields[25].toInt();
  return c;
}

// V7: + personAlertsEnabled, vehicleAlertsEnabled = 28. An empty field means
// true here (the opt-out default), unlike the other bools.
static CameraConfig deserializeCameraV7(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() != 28) return c; // malformed - caller skips entries with an empty name

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
  if (fields[11].length() > 0) c.alertCooldownMs    = (unsigned long)fields[11].toInt();
  if (fields[12].length() > 0) c.offlineThresholdMs  = (unsigned long)fields[12].toInt();
  if (fields[13].length() > 0) c.snapshotBurstCount  = (unsigned int)fields[13].toInt();
  c.quietHoursEnabled              = fields[14] == "1";
  if (fields[15].length() > 0) c.quietStartMinute    = (uint16_t)fields[15].toInt();
  if (fields[16].length() > 0) c.quietEndMinute      = (uint16_t)fields[16].toInt();
  if (fields[17].length() > 0) c.motionWatchdogHours = (uint16_t)fields[17].toInt();
  if (fields[18].length() > 0) c.timelapseIntervalMin = (uint16_t)fields[18].toInt();
  if (fields[19].length() > 0) c.pollIntervalMs      = (unsigned long)fields[19].toInt();
  c.timelapseSendToTelegram        = fields[20] == "1";
  if (fields[21].length() > 0) c.retentionDays       = (uint16_t)fields[21].toInt();
  c.petAlertsEnabled                = fields[22] == "1";
  c.petAlertsTextOnly               = fields[23] == "1";
  if (fields[24].length() > 0) c.snapshotMaxWidth    = (uint16_t)fields[24].toInt();
  if (fields[25].length() > 0) c.snapshotMaxHeight   = (uint16_t)fields[25].toInt();
  if (fields[26].length() > 0) c.personAlertsEnabled  = fields[26] == "1";
  if (fields[27].length() > 0) c.vehicleAlertsEnabled = fields[27] == "1";
  return c;
}

// V8: + motionDigestEnabled = 29 (empty also means true).
static CameraConfig deserializeCameraV8(const std::vector<String>& fields) {
  CameraConfig c;
  if (fields.size() != 29) return c; // malformed - caller skips entries with an empty name

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
  if (fields[11].length() > 0) c.alertCooldownMs    = (unsigned long)fields[11].toInt();
  if (fields[12].length() > 0) c.offlineThresholdMs  = (unsigned long)fields[12].toInt();
  if (fields[13].length() > 0) c.snapshotBurstCount  = (unsigned int)fields[13].toInt();
  c.quietHoursEnabled              = fields[14] == "1";
  if (fields[15].length() > 0) c.quietStartMinute    = (uint16_t)fields[15].toInt();
  if (fields[16].length() > 0) c.quietEndMinute      = (uint16_t)fields[16].toInt();
  if (fields[17].length() > 0) c.motionWatchdogHours = (uint16_t)fields[17].toInt();
  if (fields[18].length() > 0) c.timelapseIntervalMin = (uint16_t)fields[18].toInt();
  if (fields[19].length() > 0) c.pollIntervalMs      = (unsigned long)fields[19].toInt();
  c.timelapseSendToTelegram        = fields[20] == "1";
  if (fields[21].length() > 0) c.retentionDays       = (uint16_t)fields[21].toInt();
  c.petAlertsEnabled                = fields[22] == "1";
  c.petAlertsTextOnly               = fields[23] == "1";
  if (fields[24].length() > 0) c.snapshotMaxWidth    = (uint16_t)fields[24].toInt();
  if (fields[25].length() > 0) c.snapshotMaxHeight   = (uint16_t)fields[25].toInt();
  if (fields[26].length() > 0) c.personAlertsEnabled  = fields[26] == "1";
  if (fields[27].length() > 0) c.vehicleAlertsEnabled = fields[27] == "1";
  if (fields[28].length() > 0) c.motionDigestEnabled  = fields[28] == "1";
  return c;
}

CameraConfig deserializeCamera(const String& record, uint16_t recordVersion) {
  std::vector<String> fields = splitFields(record);

  if (recordVersion == 0) return deserializeCameraV0(fields);
  if (recordVersion == 1) return deserializeCameraV1(fields);
  if (recordVersion == 2) return deserializeCameraV2(fields);
  if (recordVersion == 3) return deserializeCameraV3(fields);
  if (recordVersion == 4) return deserializeCameraV4(fields);
  if (recordVersion == 5) return deserializeCameraV5(fields);
  if (recordVersion == 6) return deserializeCameraV6(fields);
  if (recordVersion == 7) return deserializeCameraV7(fields);
  if (recordVersion == CAMERA_SCHEMA_VERSION) return deserializeCameraV8(fields);

  // Newer than we know (likely a downgrade): try the newest layout;
  // loadCameras logs a warning.
  return deserializeCameraV8(fields);
}

size_t cameraRecordFieldCount(const String& record) {
  return splitFields(record).size();
}

void sortCamerasByName(std::vector<CameraConfig>& cams) {
  std::sort(cams.begin(), cams.end(), [](const CameraConfig& a, const CameraConfig& b) {
    String an = a.name; an.toLowerCase();
    String bn = b.name; bn.toLowerCase();
    return an < bn;
  });
}

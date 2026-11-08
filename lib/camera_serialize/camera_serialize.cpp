#include "camera_serialize.h"
#include <algorithm>
#include <type_traits>

// Field separator within one record (ASCII unit separator - never in real
// data, so no escaping). Records are joined by camera_file_store's separator.
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

namespace {

struct CameraField {
  uint16_t addedInVersion;
  void (*write)(const CameraConfig&, String&);
  void (*read)(CameraConfig&, const String&);
};

template <auto Member>
void writeText(const CameraConfig& c, String& out) { out += stripSeparators(c.*Member); }
template <auto Member>
void readText(CameraConfig& c, const String& v) { c.*Member = v; }

template <auto Member>
void writeFlag(const CameraConfig& c, String& out) { out += (c.*Member ? "1" : "0"); }
template <auto Member>
void readFlag(CameraConfig& c, const String& v) { c.*Member = v == "1"; }
// Empty keeps CameraConfig's default, for flags that default to true.
template <auto Member>
void readFlagOrDefault(CameraConfig& c, const String& v) { if (v.length() > 0) c.*Member = v == "1"; }

template <auto Member>
void writeNumber(const CameraConfig& c, String& out) { out += String(c.*Member); }
// Empty keeps CameraConfig's default.
template <auto Member>
void readNumber(CameraConfig& c, const String& v) {
  if (v.length() == 0) return;
  auto& field = c.*Member;
  field = (std::remove_reference_t<decltype(field)>)v.toInt();
}

template <auto Member>
constexpr CameraField text(uint16_t version) { return {version, writeText<Member>, readText<Member>}; }
template <auto Member>
constexpr CameraField flag(uint16_t version) { return {version, writeFlag<Member>, readFlag<Member>}; }
template <auto Member>
constexpr CameraField flagOrDefault(uint16_t version) { return {version, writeFlag<Member>, readFlagOrDefault<Member>}; }
template <auto Member>
constexpr CameraField number(uint16_t version) { return {version, writeNumber<Member>, readNumber<Member>}; }

using C = CameraConfig;

// The record layout, in order. Append only: a new setting gets a new last row
// with the next version, and CAMERA_SCHEMA_VERSION goes up to match. A record
// of version V holds exactly the rows added in V or earlier.
const CameraField kFields[] = {
    text<&C::name>(0),
    text<&C::deviceServiceUrl>(0),
    flag<&C::enabled>(0),
    flag<&C::useWSSecurity>(0),
    flag<&C::includeInitialTerminationTime>(0),
    flag<&C::includeReplyToAnonymous>(0),
    text<&C::snapshotUriOverride>(0),
    text<&C::preferredProfileKeyword>(0),
    text<&C::user>(0),
    text<&C::pass>(0),
    text<&C::notes>(0),
    number<&C::alertCooldownMs>(0),
    number<&C::offlineThresholdMs>(0),
    number<&C::snapshotBurstCount>(0),
    flag<&C::quietHoursEnabled>(2),
    number<&C::quietStartMinute>(2),
    number<&C::quietEndMinute>(2),
    number<&C::motionWatchdogHours>(2),
    number<&C::timelapseIntervalMin>(2),
    number<&C::pollIntervalMs>(3),
    flag<&C::timelapseSendToTelegram>(4),
    number<&C::retentionDays>(4),
    flag<&C::petAlertsEnabled>(5),
    flag<&C::petAlertsTextOnly>(5),
    number<&C::snapshotMaxWidth>(6),
    number<&C::snapshotMaxHeight>(6),
    flagOrDefault<&C::personAlertsEnabled>(7),
    flagOrDefault<&C::vehicleAlertsEnabled>(7),
    flagOrDefault<&C::motionDigestEnabled>(8),
};
const size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

// V0 (before versioning) grew by appending, so 11 to 14 fields are all valid;
// from V1 on the count must match exactly.
const size_t kV0MinFields = 11;

size_t fieldsInVersion(uint16_t version) {
  size_t count = 0;
  while (count < kFieldCount && kFields[count].addedInVersion <= version) count++;
  return count;
}

}  // namespace

String serializeCamera(const CameraConfig& c) {
  String s;
  for (size_t i = 0; i < kFieldCount; i++) {
    if (i > 0) s += FIELD_SEP;
    kFields[i].write(c, s);
  }
  return s;
}

CameraConfig deserializeCamera(const String& record, uint16_t recordVersion) {
  // Newer than we know (likely a downgrade): try the newest layout;
  // loadCameras logs a warning.
  uint16_t version = std::min(recordVersion, CAMERA_SCHEMA_VERSION);
  std::vector<String> fields = splitFields(record);
  size_t expected = fieldsInVersion(version);

  CameraConfig c;
  if (version == 0 ? fields.size() < kV0MinFields : fields.size() != expected) {
    return c; // malformed - caller skips entries with an empty name
  }
  for (size_t i = 0; i < fields.size() && i < expected; i++) kFields[i].read(c, fields[i]);
  return c;
}

size_t cameraRecordFieldCount(const String& record) {
  return splitFields(record).size();
}

String cameraRecordExpectedFields(uint16_t recordVersion) {
  uint16_t version = std::min(recordVersion, CAMERA_SCHEMA_VERSION);
  String expected((unsigned)fieldsInVersion(version));
  return version == 0 ? String((unsigned)kV0MinFields) + "-" + expected : expected;
}

void sortCamerasByName(std::vector<CameraConfig>& cams) {
  std::sort(cams.begin(), cams.end(), [](const CameraConfig& a, const CameraConfig& b) {
    String an = a.name; an.toLowerCase();
    String bn = b.name; bn.toLowerCase();
    return an < bn;
  });
}

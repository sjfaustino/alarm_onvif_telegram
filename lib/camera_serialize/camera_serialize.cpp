#include "camera_serialize.h"
#include "record_table.h"
#include <algorithm>

namespace {

using C = CameraConfig;

// The record layout, in order. Append only: a new setting gets a new last row
// with the next version, and CAMERA_SCHEMA_VERSION goes up to match. A record
// of version V holds exactly the rows added in V or earlier.
const RecordField<CameraConfig> kFields[] = {
    recordText<&C::name>(0),
    recordText<&C::deviceServiceUrl>(0),
    recordFlag<&C::enabled>(0),
    recordFlag<&C::useWSSecurity>(0),
    recordFlag<&C::includeInitialTerminationTime>(0),
    recordFlag<&C::includeReplyToAnonymous>(0),
    recordText<&C::snapshotUriOverride>(0),
    recordText<&C::preferredProfileKeyword>(0),
    recordText<&C::user>(0),
    recordText<&C::pass>(0),
    recordText<&C::notes>(0),
    recordNumber<&C::alertCooldownMs>(0),
    recordNumber<&C::offlineThresholdMs>(0),
    recordNumber<&C::snapshotBurstCount>(0),
    recordFlag<&C::quietHoursEnabled>(2),
    recordNumber<&C::quietStartMinute>(2),
    recordNumber<&C::quietEndMinute>(2),
    recordNumber<&C::motionWatchdogHours>(2),
    recordNumber<&C::timelapseIntervalMin>(2),
    recordNumber<&C::pollIntervalMs>(3),
    recordFlag<&C::timelapseSendToTelegram>(4),
    recordNumber<&C::retentionDays>(4),
    recordFlag<&C::petAlertsEnabled>(5),
    recordFlag<&C::petAlertsTextOnly>(5),
    recordNumber<&C::snapshotMaxWidth>(6),
    recordNumber<&C::snapshotMaxHeight>(6),
    recordFlagOrDefault<&C::personAlertsEnabled>(7),
    recordFlagOrDefault<&C::vehicleAlertsEnabled>(7),
    recordFlagOrDefault<&C::motionDigestEnabled>(8),
};

// V0 (before versioning) grew by appending, so 11 to 14 fields are all valid.
const size_t kV0MinFields = 11;

}  // namespace

String serializeCamera(const CameraConfig& c) { return writeRecord(kFields, c); }

CameraConfig deserializeCamera(const String& record, uint16_t recordVersion) {
  return readRecord(kFields, record, recordVersion, kV0MinFields); // loadCameras warns if newer
}

size_t cameraRecordFieldCount(const String& record) { return splitRecordFields(record).size(); }

String cameraRecordExpectedFields(uint16_t recordVersion) {
  String expected((unsigned)recordFieldsInVersion(kFields, recordVersion));
  return recordVersion == 0 ? String((unsigned)kV0MinFields) + "-" + expected : expected;
}

void sortCamerasByName(std::vector<CameraConfig>& cams) {
  std::sort(cams.begin(), cams.end(), [](const CameraConfig& a, const CameraConfig& b) {
    String an = a.name; an.toLowerCase();
    String bn = b.name; bn.toLowerCase();
    return an < bn;
  });
}

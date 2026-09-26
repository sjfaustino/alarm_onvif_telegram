#pragma once
#include <Arduino.h>
#include <time.h>

// Persisted record of the most recent OTA update's outcome (boot_checks.cpp's
// end-of-setup() checks) - lets the Firmware page show "last update
// confirmed healthy" / "last update rolled back" instead of that
// information only ever existing as a one-time Telegram push/Activity-log
// line that's easy to miss and impossible to check back on later. Also
// owns the small dedup marker those same checks use to avoid re-alerting
// about the same rollback on every later boot - both concerns share one
// "otastate" NVS namespace, kept together here instead of boot_checks.cpp poking
// at raw Preferences calls directly.

enum class OtaOutcome { None, Healthy, RolledBack };

struct OtaHistoryEntry {
  OtaOutcome outcome = OtaOutcome::None; // None = no OTA event has ever been recorded
  String partitionLabel;                 // which OTA partition this outcome was about
  time_t epoch = 0;                      // wall-clock time it was recorded; 0 if unknown
};

// For the Firmware page - whatever recordOtaOutcome most recently wrote,
// persisted across reboots. {None, "", 0} if nothing's ever been recorded.
OtaHistoryEntry loadOtaHistory();

// Called once, right when boot_checks.cpp's boot-time checks confirm a real
// event (never on an ordinary boot where nothing changed) - overwrites
// whatever was recorded before with outcome/partitionLabel/now().
void recordOtaOutcome(OtaOutcome outcome, const String& partitionLabel);

// The dedup marker confirmFirmwareAndReportRollback (boot_checks.cpp) uses to avoid re-alerting
// about the same rollback on every later boot -
// esp_ota_get_last_invalid_partition() stays non-null until that slot is
// overwritten by another OTA attempt, unlike the mark-valid call's own
// naturally one-shot PENDING_VERIFY check for the success path.
bool alreadyReportedInvalidPartition(const String& label);
void rememberReportedInvalidPartition(const String& label);
void forgetReportedInvalidPartition();

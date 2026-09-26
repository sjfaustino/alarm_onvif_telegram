#pragma once
#include <Arduino.h>
#include <time.h>

// Last OTA outcome, persisted so the Firmware page can show it, plus the
// marker that stops a rollback being re-reported every boot. Both live in the
// "otastate" NVS namespace.

enum class OtaOutcome { None, Healthy, RolledBack };

struct OtaHistoryEntry {
  OtaOutcome outcome = OtaOutcome::None; // None = no OTA event has ever been recorded
  String partitionLabel;                 // which OTA partition this outcome was about
  time_t epoch = 0;                      // wall-clock time it was recorded; 0 if unknown
};

// {None, "", 0} if nothing recorded.
OtaHistoryEntry loadOtaHistory();

// Call only when a boot actually confirms or rolls back an update.
void recordOtaOutcome(OtaOutcome outcome, const String& partitionLabel);

// Dedup for rollback alerts: the invalid partition stays reported until
// another OTA overwrites that slot.
bool alreadyReportedInvalidPartition(const String& label);
void rememberReportedInvalidPartition(const String& label);
void forgetReportedInvalidPartition();

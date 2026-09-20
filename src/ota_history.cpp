#include "ota_history.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "otastate";
static const char* NVS_KEY_OUTCOME = "outcome";       // "healthy" | "rolledback"
static const char* NVS_KEY_PARTITION = "partition";
static const char* NVS_KEY_EPOCH = "epoch";
static const char* NVS_KEY_LAST_INVALID = "invalidPart";

OtaHistoryEntry loadOtaHistory() {
  OtaHistoryEntry entry;
  Preferences prefs;
  // Read-write, not read-only, even though this never calls put*() - see
  // auth_store.cpp's loadDashboardAuth for why: a read-only open against
  // a namespace that's never been written (the common case on a board
  // that's never had an OTA update at all) fails and spams an NVS error
  // log on every single call otherwise.
  prefs.begin(NVS_NAMESPACE, false);
  String outcome = prefs.getString(NVS_KEY_OUTCOME, "");
  if (outcome == "healthy") entry.outcome = OtaOutcome::Healthy;
  else if (outcome == "rolledback") entry.outcome = OtaOutcome::RolledBack;
  entry.partitionLabel = prefs.getString(NVS_KEY_PARTITION, "");
  entry.epoch = (time_t)prefs.getULong64(NVS_KEY_EPOCH, 0);
  prefs.end();
  return entry;
}

void recordOtaOutcome(OtaOutcome outcome, const String& partitionLabel) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putString(NVS_KEY_OUTCOME, outcome == OtaOutcome::Healthy ? "healthy" : "rolledback");
  prefs.putString(NVS_KEY_PARTITION, partitionLabel);
  time_t now;
  time(&now);
  prefs.putULong64(NVS_KEY_EPOCH, (uint64_t)now);
  prefs.end();
}

bool alreadyReportedInvalidPartition(const String& label) {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false); // read-write - same reasoning as loadOtaHistory above
  String last = prefs.getString(NVS_KEY_LAST_INVALID, "");
  prefs.end();
  return last == label;
}

void rememberReportedInvalidPartition(const String& label) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putString(NVS_KEY_LAST_INVALID, label);
  prefs.end();
}

void forgetReportedInvalidPartition() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.remove(NVS_KEY_LAST_INVALID);
  prefs.end();
}

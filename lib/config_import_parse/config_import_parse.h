#pragma once
#include <Arduino.h>
#include <vector>
#include "camera_serialize.h"
#include "telegram_user_serialize.h"
#include "network_serialize.h"
#include "sd_store.h" // SdSettings
#include "net_watchdog.h" // NetWatchdogSettings
#include "bridge_watchdog.h" // BridgeWatchdogSettings
#include "power_monitor.h" // PowerMonitorSettings

// Parses the machine-readable blocks buildConfigExport appends to an export
// (applying them is config_backup.cpp's job). Each block is a "### <SECTION>
// v<N>" marker followed by records until the next marker, so the surrounding
// prose can change freely. <N> is passed to the record deserializers, so older
// exports still parse.
struct ConfigImportResult {
  // Section present. Empty cameras with this true means the export had none
  // (or see camerasDuplicateName). Malformed records are skipped.
  bool camerasFound = false;
  std::vector<CameraConfig> cameras;

  // Two cameras share a name (case-insensitive). Every lookup is
  // first-match-by-name, so one would be orphaned; the whole section is left
  // empty so the caller can say why.
  bool camerasDuplicateName = false;

  bool usersFound = false;
  std::vector<TelegramUser> users;

  // Same for users sharing a name or chat ID (double alerts, ambiguous
  // permissions).
  bool usersDuplicateIdentity = false;

  // Single-record sections count as found only if the line parsed: applying a
  // blank record would wipe good config.
  bool networkFound = false;
  WifiCredentials network; // both .password fields always blank - never serialized

  bool sdSettingsFound = false;
  SdSettings sdSettings;

  bool netWatchdogFound = false;
  NetWatchdogSettings netWatchdogSettings;

  bool bridgeWatchdogFound = false;
  BridgeWatchdogSettings bridgeWatchdogSettings;

  bool powerMonitorFound = false;
  PowerMonitorSettings powerMonitorSettings;
};

ConfigImportResult parseConfigImport(const String& text);

#pragma once
#include <Arduino.h>
#include <vector>
#include "camera_serialize.h"
#include "telegram_user_serialize.h"
#include "network_serialize.h"
#include "sd_store.h" // SdSettings

// Pure text-in/struct-out parser for the machine-readable blocks
// buildConfigExport() (webserver_security.cpp) appends after each of its
// prose sections - the other half of the config export/import feature.
// Split out so it's natively unit-testable (test/test_config_import_parse)
// without <Preferences.h>, which only exists on-device; applying the
// result to NVS (saveCameras/saveTelegramUsers/saveWifiCredentials/
// saveSdSettings) is webserver_security.cpp's job, not this one's.
//
// Each block is self-delimiting: a "### <SECTION> v<N>" marker line, then
// data lines (camera_serialize.h/telegram_user_serialize.h/
// network_serialize.h record format) until the next marker or end of
// input - never dependent on the surrounding prose wording, which can
// (and has) changed across firmware versions. <N> is passed straight
// through as the record's schema version to deserializeCamera/
// deserializeUser/deserializeNetworkConfig, so an export taken under an
// older schema version still parses correctly today - the exact same
// version-tolerance those functions already provide for NVS records.
struct ConfigImportResult {
  // true if a "### CAMERAS vN" marker was found at all - an empty
  // `cameras` vector with this true is a legitimate "this export had zero
  // cameras", not "no Cameras section present". Malformed individual
  // records within the section are silently skipped (same convention
  // camera_store.cpp's own loadCameras() uses), not treated as absence of
  // the whole section.
  bool camerasFound = false;
  std::vector<CameraConfig> cameras;

  bool usersFound = false;
  std::vector<TelegramUser> users;

  // Single-record domains - unlike the two lists above, an empty/malformed
  // NETWORK or SDSETTINGS line is NOT trusted as "found" (there's no
  // partial-list middle ground to fall back on, and applying a blank
  // WifiCredentials/SdSettings would actively wipe good config with
  // garbage) - found is only true once the one line actually parsed.
  bool networkFound = false;
  WifiCredentials network; // both .password fields always blank - never serialized

  bool sdSettingsFound = false;
  SdSettings sdSettings;
};

ConfigImportResult parseConfigImport(const String& text);

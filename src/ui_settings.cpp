#include "ui_settings.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "uisettings";
static const char* NVS_KEY_DARK_MODE = "darkMode";

UiSettings loadUiSettings() {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(NVS_NAMESPACE, false);
  UiSettings settings;
  settings.darkMode = prefs.getBool(NVS_KEY_DARK_MODE, false);
  prefs.end();
  return settings;
}

bool saveUiSettings(const UiSettings& settings) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBool(NVS_KEY_DARK_MODE, settings.darkMode) > 0;
  prefs.end();
  if (!ok) {
    Serial.println("[ui_settings] ERROR: failed to persist the dark mode setting to NVS - it will "
                    "revert to the previous value on the next reboot.");
  }
  return ok;
}

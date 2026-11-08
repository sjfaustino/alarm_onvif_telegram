#include "ui_settings.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "uisettings";
static const char* NVS_KEY_DARK_MODE = "darkMode";

// Read on every page render. Only the web server's one task calls these.
static UiSettings g_cached;
static bool g_cacheLoaded = false;

UiSettings loadUiSettings() {
  if (g_cacheLoaded) return g_cached;
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(NVS_NAMESPACE, false);
  g_cached.darkMode = prefs.getBool(NVS_KEY_DARK_MODE, false);
  prefs.end();
  g_cacheLoaded = true;
  return g_cached;
}

bool saveUiSettings(const UiSettings& settings) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBool(NVS_KEY_DARK_MODE, settings.darkMode) > 0;
  prefs.end();
  if (ok) {
    g_cached = settings;
    g_cacheLoaded = true;
  } else {
    Serial.println("[ui_settings] ERROR: failed to persist the dark mode setting to NVS - it will "
                    "revert to the previous value on the next reboot.");
  }
  return ok;
}

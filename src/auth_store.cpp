#include "auth_store.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "dashauth";
static const char* NVS_KEY_USER  = "user";
static const char* NVS_KEY_PASS  = "pass";

DashboardAuth loadDashboardAuth() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  DashboardAuth auth;
  auth.username = prefs.getString(NVS_KEY_USER, "");
  auth.password = prefs.getString(NVS_KEY_PASS, "");
  prefs.end();
  return auth;
}

bool saveDashboardAuth(const DashboardAuth& auth) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  // putString returns bytes written, 0 on failure - comparing against the
  // source string's own length (rather than just "> 0") correctly treats a
  // legitimately empty value as success too, not just a non-empty one.
  bool userOk = prefs.putString(NVS_KEY_USER, auth.username) == auth.username.length();
  bool passOk = prefs.putString(NVS_KEY_PASS, auth.password) == auth.password.length();
  prefs.end();
  if (!userOk || !passOk) {
    // Same failure class camera_store.cpp's saveCameras hit in the field
    // (NVS full/write error silently ignored) - here it means the dashboard
    // login the caller believes it just set was never actually persisted,
    // and will revert to whatever's really on flash on the next reboot.
    Serial.println("[auth_store] ERROR: failed to persist dashboard login to NVS - it will revert "
                    "to the previous value on the next reboot.");
    return false;
  }
  return true;
}

#include "auth_store.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "dashauth";
static const char* NVS_KEY_USER  = "user";
static const char* NVS_KEY_PASS  = "pass";

DashboardAuth loadDashboardAuth() {
  Preferences prefs;
  // Read-write even though nothing is written here: a read-only open of a
  // never-written namespace logs a NOT_FOUND error on every page render. This
  // creates it (empty) once.
  prefs.begin(NVS_NAMESPACE, false);
  // Likewise getString() on a missing key logs an error on every call, while
  // isKey() is silent - so seed empty values once. The effective value is
  // unchanged.
  if (!prefs.isKey(NVS_KEY_USER)) prefs.putString(NVS_KEY_USER, "");
  if (!prefs.isKey(NVS_KEY_PASS)) prefs.putString(NVS_KEY_PASS, "");
  DashboardAuth auth;
  auth.username = prefs.getString(NVS_KEY_USER, "");
  auth.password = prefs.getString(NVS_KEY_PASS, "");
  prefs.end();
  return auth;
}

bool saveDashboardAuth(const DashboardAuth& auth) {
  // For rolling back a partial failure.
  DashboardAuth previous = loadDashboardAuth();

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  // Compare with the length so an empty value counts as success.
  bool userOk = prefs.putString(NVS_KEY_USER, auth.username) == auth.username.length();
  bool passOk = prefs.putString(NVS_KEY_PASS, auth.password) == auth.password.length();

  // Two independent keys: if only one write succeeds, roll it back so the
  // stored pair is never half old, half new.
  if (userOk && !passOk) {
    prefs.putString(NVS_KEY_USER, previous.username);
  } else if (!userOk && passOk) {
    prefs.putString(NVS_KEY_PASS, previous.password);
  }
  prefs.end();

  if (!userOk || !passOk) {
    // Unsaved logins silently revert on the next reboot, so say so.
    Serial.println("[auth_store] ERROR: failed to persist dashboard login to NVS - reverted to the "
                    "previous username/password pair so they can't end up mismatched.");
    return false;
  }
  return true;
}

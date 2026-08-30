#include "auth_store.h"
#include <Preferences.h>

static const char* NVS_NAMESPACE = "dashauth";
static const char* NVS_KEY_USER  = "user";
static const char* NVS_KEY_PASS  = "pass";

DashboardAuth loadDashboardAuth() {
  Preferences prefs;
  // Opened read-write, even though nothing here ever calls put*() - a
  // read-only open against a namespace that's never been written (the
  // common case here: no dashboard password ever set) fails with
  // ESP_ERR_NVS_NOT_FOUND, which the framework itself logs as an
  // "[E][Preferences.cpp] nvs_open failed: NOT_FOUND" error on EVERY
  // single request (this is called from renderShell(), once per page
  // load). A read-write open instead lazily creates the (still-empty)
  // namespace the first time this runs, silencing that spam for good -
  // getString's own defaults below still apply either way.
  prefs.begin(NVS_NAMESPACE, false);
  DashboardAuth auth;
  auth.username = prefs.getString(NVS_KEY_USER, "");
  auth.password = prefs.getString(NVS_KEY_PASS, "");
  prefs.end();
  return auth;
}

bool saveDashboardAuth(const DashboardAuth& auth) {
  // Read before writing so a partial failure below can be rolled back to
  // this, not left half-applied.
  DashboardAuth previous = loadDashboardAuth();

  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  // putString returns bytes written, 0 on failure - comparing against the
  // source string's own length (rather than just "> 0") correctly treats a
  // legitimately empty value as success too, not just a non-empty one.
  bool userOk = prefs.putString(NVS_KEY_USER, auth.username) == auth.username.length();
  bool passOk = prefs.putString(NVS_KEY_PASS, auth.password) == auth.password.length();

  // Username and password are two independent NVS keys, not one atomic
  // write - if only one of the two succeeds, NVS would otherwise be left
  // with the NEW value for one and the OLD value for the other, a
  // mismatched pair the caller is never told about (it's reported as a
  // failed save, but a half-applied one still silently changes what
  // credentials the board actually enforces on the very next request).
  // Roll the succeeded half back to its previous value so the persisted
  // pair always stays either fully new or fully old, never mixed.
  if (userOk && !passOk) {
    prefs.putString(NVS_KEY_USER, previous.username);
  } else if (!userOk && passOk) {
    prefs.putString(NVS_KEY_PASS, previous.password);
  }
  prefs.end();

  if (!userOk || !passOk) {
    // Same failure class camera_store.cpp's saveCameras hit in the field
    // (NVS full/write error silently ignored) - here it means the dashboard
    // login the caller believes it just set was never actually persisted,
    // and will revert to whatever's really on flash on the next reboot.
    Serial.println("[auth_store] ERROR: failed to persist dashboard login to NVS - reverted to the "
                    "previous username/password pair so they can't end up mismatched.");
    return false;
  }
  return true;
}

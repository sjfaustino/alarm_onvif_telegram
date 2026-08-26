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
  prefs.putString(NVS_KEY_USER, auth.username);
  prefs.putString(NVS_KEY_PASS, auth.password);
  prefs.end();
  return true;
}

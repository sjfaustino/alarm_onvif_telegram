#pragma once
#include <Arduino.h>

// Dashboard login (HTTP Basic Auth) - persisted in NVS (Preferences,
// namespace "dashauth"), managed at runtime via the web UI's Security
// section. Both fields default to empty, which means "no login required" -
// the board boots wide open and stays that way until someone sets a
// username/password from the dashboard itself (see webserver.cpp's
// AuthenticationMiddleware setup, which only requires auth once both are
// non-empty). There's no separate "enabled" flag - emptiness *is* disabled.
struct DashboardAuth {
  String username;
  String password;
};

// Loads the current dashboard login from NVS - empty fields if never set.
DashboardAuth loadDashboardAuth();

// Overwrites the persisted dashboard login. Passing empty username/password
// disables the login requirement again.
bool saveDashboardAuth(const DashboardAuth& auth);

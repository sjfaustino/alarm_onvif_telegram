#pragma once
#include <Arduino.h>

// Dashboard Basic Auth login, NVS "dashauth". Empty (the default) means no
// login required - there is no separate enabled flag.
struct DashboardAuth {
  String username;
  String password;
};

DashboardAuth loadDashboardAuth();

// Empty username/password disables the login again.
bool saveDashboardAuth(const DashboardAuth& auth);

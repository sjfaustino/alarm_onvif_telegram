#pragma once
#include <Arduino.h>

// Maintenance panel content (currently just a manual reboot button). Split
// out of webserver.cpp - see webserver_network.h's comment for why. The
// actual reboot (a delayed ESP.restart() from a short-lived task, so the
// response finishes sending first) stays in webserver.cpp, alongside the
// same pattern the Firmware page's OTA success path already uses.
//
// The Internet Watchdog and Camera Bridge Watchdog fieldsets that used to
// live here moved to their own pages under the Hardware sidebar menu -
// see webserver_hardware.h.
String renderMaintenancePanel();

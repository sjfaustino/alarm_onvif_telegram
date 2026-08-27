#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>

// Network panel: WiFi status, network config form (primary/backup SSID,
// hostname, static IP vs DHCP, NTP server, POSIX TZ). Split out of
// webserver.cpp, which was a single 946-line file mixing every panel's
// rendering, form-parsing, and business logic with the actual routing
// table - see webserver.cpp's startWebServer() for how the content
// returned here plugs into the dashboard shell/routing.

String renderNetworkPanel();

// Applies the /network/save form to storage. banner receives the
// success/failure message to show the user.
void handleSaveNetwork(PsychicRequest* request, String& banner);

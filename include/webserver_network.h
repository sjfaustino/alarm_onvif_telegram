#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>

// Network panel: WiFi status, network config form (primary/backup SSID,
// hostname, static IP vs DHCP, NTP server, POSIX TZ). Split out of
// webserver.cpp, which was a single 946-line file mixing every panel's
// rendering, form-parsing, and business logic with the actual routing
// table - see webserver.cpp's startWebServer() for how the content
// returned here plugs into the dashboard shell/routing.

// prefillSsid overrides the Primary SSID field's value for this render
// only (not persisted) - used by the "Search WiFi networks" results'
// Add link (via /network?prefillSsid=...) the same way the Cameras page's
// discovered-camera Add link prefills its own Add form. "" renders the
// stored primary SSID as usual.
String renderNetworkPanel(const String& prefillSsid = "");

// Applies the /network/save form to storage. banner receives the
// success/failure message to show the user.
void handleSaveNetwork(PsychicRequest* request, String& banner);

// ============================================================
// WiFi network scan - see wifi_scan.h (lib/) for the pure result-list
// dedupe/sort logic this wraps with the actual WiFi.scanNetworks() call
// and background-task glue, same split as camera discovery's
// onvif_discovery.h/webserver_cameras.cpp.
// ============================================================

// Starts a WiFi scan on a background FreeRTOS task instead of the calling
// (PsychicHttp) task - WiFi.scanNetworks() blocks for a couple of seconds
// and, since this board is already connected as a station, briefly
// interrupts its own traffic while it hops channels; running it
// synchronously on the request-handling task risks stalling the very
// request that triggered it, on top of blocking every other page load for
// the scan's duration. A no-op if a scan is already in progress. Call this
// from the /network/scan route handler.
void startWifiScanAsync();

// Renders the current scan status: "a scan is running" while one is in
// progress, the last completed scan's results (a table with an Add link
// per network, prefilling the Primary SSID field below) once one exists,
// or "" if no scan has run yet this boot. Safe to call from any task
// (internally locked) - renderNetworkPanel calls this itself.
String renderWifiScanStatus();

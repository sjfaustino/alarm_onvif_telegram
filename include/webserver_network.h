#pragma once
#include <Arduino.h>
#include <PsychicHttp.h>
#include "background_job.h" // BackgroundJobStartOutcome

// Network panel: WiFi status and the network settings form.

// prefillSsid fills the Primary SSID field for this render only (the scan
// results' Add link); "" shows the stored value.
String renderNetworkPanel(const String& prefillSsid = "");

void handleSaveNetwork(PsychicRequest* request, String& banner);

// ============================================================
// WiFi scan; lib/wifi_scan holds the dedupe/sort logic.
// ============================================================

// Starts a scan on a task: scanNetworks() blocks for seconds and briefly
// interrupts the station's own traffic.
BackgroundJobStartOutcome startWifiScanAsync();

// Scan status HTML: running, results with Add links, or "".
String renderWifiScanStatus();

// True while a scan runs, so the page auto-refreshes.
bool networkJobsInProgress();

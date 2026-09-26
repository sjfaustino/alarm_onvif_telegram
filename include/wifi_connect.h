#pragma once
#include "network_store.h" // WifiCredentials

// Loaded by setup(); read by setupTime() and startMonitoring(). connectWiFi()
// may swap primary/backup in it (and persist that) when only backup works.
extern WifiCredentials g_wifiCredentials;

// Tries primary, then backup. Blocks up to ~30s per network, feeding the
// task watchdog while it waits.
void connectWiFi();

// loop()'s reconnect backoff: an attempt is due once the backoff delay since
// the last failure has elapsed (immediately if nothing has failed yet).
bool wifiReconnectDue();
// Resets the backoff on success; on failure doubles it (capped) and logs.
void recordWifiReconnectResult(bool connected);

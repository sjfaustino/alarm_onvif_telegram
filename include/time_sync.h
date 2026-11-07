#pragma once

// Seeds the system clock from the external RTC, if one is active. Call after
// initRtc() and before WiFi/NTP.
void seedSystemClockFromRtc();

// Call regularly from loop() while WiFi is up: retries a failed or stale NTP
// setup (see ntp_policy.h).
void maintainTimeSync();

// NTP sync against g_wifiCredentials' server and TZ. On failure, falls back
// to the router's HTTP Date header (only if there's no RTC) and alerts once
// per outage.
void setupTime();

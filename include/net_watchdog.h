#pragma once
#include <Arduino.h>
#include "config.h" // NET_WATCHDOG_PIN_DEFAULT

// Optional internet-connectivity watchdog - same "optional peripheral, off
// by default, graceful fallback" shape as sd_store.h/rtc_store.h. Tests
// actual WAN reachability (not just WiFi.status()==WL_CONNECTED, which
// only confirms the WiFi-to-router hop - a 4G/LTE router can lose its
// uplink and never recover while that stays up the whole time) and, once
// an outage has lasted longer than a configurable threshold, pulses a
// relay wired in series with the router's own power to force a
// power-cycle. See config.h's own comment on this feature for the pin-
// safety reasoning.

struct NetWatchdogSettings {
  bool enabled = false;
  int pin = NET_WATCHDOG_PIN_DEFAULT;
  bool activeLow = true; // most relay modules are active-low - LOW = energized/closed
  uint32_t outageThresholdMs = 5UL * 60UL * 1000UL; // 5 minutes
  uint32_t pulseDurationMs = 10UL * 1000UL;         // 10 seconds
};
NetWatchdogSettings loadNetWatchdogSettings();
bool saveNetWatchdogSettings(const NetWatchdogSettings& settings);

// Call once from setup() - sets pinMode(OUTPUT) and drives the pin to its
// resting ("router powered") state if enabled AND the configured pin
// passes isReservedOrUnsafePin's check (lib/net_watchdog_logic). Serial-
// only warning and stays inactive otherwise - a bad pin choice is a setup
// mistake to fix and reboot from, not a runtime data-loss risk worth a
// Telegram alert (same tone as the RTC's own init-failure path).
void initNetWatchdog();

// True only if the setting is enabled AND initNetWatchdog() accepted the
// configured pin - mirrors rtcActive()/sdActive()'s shape.
bool netWatchdogActive();

// Call on NET_WATCHDOG_CHECK_INTERVAL_MS's own cadence (main.cpp's
// loop()) - does the actual WAN-reachability probe (a short-timeout raw
// TCP connect to a fixed, well-known IP:port - deliberately not DNS-
// based, since DNS itself needs working WAN to resolve anything), and
// runs the outage-threshold/pulse decision. Returns true exactly once per
// detected outage - right when the relay was just pulsed - so the caller
// knows to send the Telegram alert; false every other call (no-op if
// !netWatchdogActive(), still within the threshold, or already recovered).
bool checkInternetAndMaybePulseRelay();

// Status for the dashboard (Maintenance page) - reflects live state,
// doesn't re-probe.
struct NetWatchdogStatus {
  bool settingEnabled = false;
  bool available = false;       // netWatchdogActive()'s value
  bool outageInProgress = false;
  unsigned long outageStartMs = 0; // millis() timestamp; meaningful only if outageInProgress
};
NetWatchdogStatus getNetWatchdogStatus();

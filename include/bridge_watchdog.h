#pragma once
#include <Arduino.h>
#include "config.h" // BRIDGE_WATCHDOG_PIN_DEFAULT
#include "camera_store.h" // CameraConfig
#include "camera.h" // CameraState

// Optional camera-bridge watchdog - same "optional peripheral, off by
// default, graceful fallback" shape as net_watchdog.h/sd_store.h/
// rtc_store.h. Watches two specific configured cameras (matched by name)
// and, once BOTH have been CameraState::isOffline for longer than a
// configurable threshold, pulses a relay wired to the local wireless
// bridge carrying them to force a power-cycle - the same recovery
// mechanism net_watchdog.h uses for the board's own WAN link, applied to a
// different failure signal. See config.h's own comment on this feature
// for the pin-safety/pin-conflict reasoning.

struct BridgeWatchdogSettings {
  bool enabled = false;
  String cameraA;   // matched against CameraConfig::name, case-insensitively
  String cameraB;
  int pin = BRIDGE_WATCHDOG_PIN_DEFAULT;
  bool activeLow = true; // most relay modules are active-low - LOW = energized/closed
  uint32_t outageThresholdMs = 5UL * 60UL * 1000UL; // 5 minutes
  uint32_t pulseDurationMs = 10UL * 1000UL;         // 10 seconds
};
BridgeWatchdogSettings loadBridgeWatchdogSettings();
bool saveBridgeWatchdogSettings(const BridgeWatchdogSettings& settings);

// Call once from setup() - sets pinMode(OUTPUT) and drives the pin to its
// resting ("bridge powered") state if enabled AND the configured pin
// passes isReservedOrUnsafePin's check AND doesn't conflict
// (watchdogPinsConflict) with the currently-saved NetWatchdogSettings -
// this last check is defense in depth, the dashboard save routes
// (webserver.cpp) are the primary guard. Serial-only warning and stays
// inactive otherwise - a bad config is a setup mistake to fix and reboot
// from, not a runtime data-loss risk worth a Telegram alert (same tone as
// net_watchdog's own init-failure path).
void initBridgeWatchdog();

// True only if the setting is enabled AND initBridgeWatchdog() accepted
// the configured pin - mirrors netWatchdogActive()'s shape.
bool bridgeWatchdogActive();

// Call on BRIDGE_WATCHDOG_CHECK_INTERVAL_MS's own cadence (main.cpp's
// loop()). Looks up cameraA/cameraB by name (case-insensitively) in the
// live camera list; if either name isn't currently found or that camera
// is disabled, the pair can't be evaluated - returns false without
// touching the outage timer (fail-safe: never guess), surfaced via
// getBridgeWatchdogStatus()'s camerasResolved field rather than silently
// doing nothing. Otherwise runs the same reachable/outage/pulse state
// machine as checkInternetAndMaybePulseRelay, with "both isOffline" in
// place of "WAN unreachable". Returns true exactly once per detected
// outage - right when the relay was just pulsed - so the caller knows to
// send the Telegram alert.
bool checkBridgeCamerasAndMaybePulseRelay(const CameraConfig cameras[], CameraState states[], size_t numCameras);

// Status for the dashboard (Hardware > WiFi Bridge page) - reflects live
// state, doesn't re-probe.
struct BridgeWatchdogStatus {
  bool settingEnabled = false;
  bool available = false;       // bridgeWatchdogActive()'s value
  bool camerasResolved = false; // false if cameraA/cameraB name lookup is currently failing
  bool outageInProgress = false;
  unsigned long outageStartMs = 0; // millis() timestamp; meaningful only if outageInProgress
};
BridgeWatchdogStatus getBridgeWatchdogStatus();

#pragma once
#include <Arduino.h>
#include "config.h" // BRIDGE_WATCHDOG_PIN_DEFAULT
#include "camera_store.h" // CameraConfig

// Forward-declared: camera.h needs FreeRTOS, which native tests don't have,
// and lib/config_import_parse includes this header.
struct CameraState;

// Optional bridge watchdog, off by default: when both configured cameras
// (matched by name) have been offline past a threshold, pulses a relay to
// power-cycle the wireless bridge carrying them.

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

// Call once from setup(). Drives the relay to its resting state if enabled and
// the pin is safe and doesn't clash with the internet watchdog's (the
// dashboard is the primary guard). A bad config just logs to Serial and stays
// inactive.
void initBridgeWatchdog();

// Enabled and the pin was accepted.
bool bridgeWatchdogActive();

// Pulses now, bypassing the state machine and leaving outage tracking alone.
// False if inactive.
bool bridgeWatchdogManualPulse();

// Like NetWatchdogCheckResult: the outage is reported once it's over.
struct BridgeWatchdogCheckResult {
  enum class Event { None, OutageDetected, Recovered } event = Event::None;
  unsigned long outageStartMs = 0;    // millis() timestamp; valid only if event == Recovered
  unsigned long outageDurationMs = 0; // valid only if event == Recovered
};

// Call every BRIDGE_WATCHDOG_CHECK_INTERVAL_MS. If either camera is missing or
// disabled, does nothing (never guesses) and reports it via camerasResolved.
// Otherwise the same outage/pulse state machine as the internet watchdog.
BridgeWatchdogCheckResult checkBridgeCamerasAndMaybePulseRelay(const CameraConfig cameras[], CameraState states[], size_t numCameras);

// Hardware page status; doesn't re-probe.
struct BridgeWatchdogStatus {
  bool settingEnabled = false;
  bool available = false;       // bridgeWatchdogActive()'s value
  bool camerasResolved = false; // false if cameraA/cameraB name lookup is currently failing
  bool outageInProgress = false;
  unsigned long outageStartMs = 0; // millis() timestamp; meaningful only if outageInProgress
};
BridgeWatchdogStatus getBridgeWatchdogStatus();

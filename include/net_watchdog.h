#pragma once
#include <Arduino.h>
#include "config.h" // NET_WATCHDOG_PIN_DEFAULT

// Optional internet watchdog, off by default. Probes real WAN reachability (a
// 4G router can lose its uplink while WiFi stays connected) and, after a
// configurable outage, pulses a relay to power-cycle the router.

struct NetWatchdogSettings {
  bool enabled = false;
  int pin = NET_WATCHDOG_PIN_DEFAULT;
  bool activeLow = true; // most relay modules are active-low - LOW = energized/closed
  uint32_t outageThresholdMs = 5UL * 60UL * 1000UL; // 5 minutes
  uint32_t pulseDurationMs = 10UL * 1000UL;         // 10 seconds
};
NetWatchdogSettings loadNetWatchdogSettings();
bool saveNetWatchdogSettings(const NetWatchdogSettings& settings);

// Call once from setup(). Drives the relay to its resting state if enabled and
// the pin is safe; otherwise logs and stays inactive.
void initNetWatchdog();

// Enabled and the pin was accepted.
bool netWatchdogActive();

// "Pulse relay now" test button: bypasses the state machine and leaves outage
// tracking alone. False if inactive.
bool netWatchdogManualPulse();

// Recovered is reported when WAN comes back rather than when the outage starts
// - an alert then would need the missing WAN. outageStartMs/ outageDurationMs
// are only meaningful for Recovered.
struct NetWatchdogCheckResult {
  enum class Event { None, OutageDetected, Recovered } event = Event::None;
  unsigned long outageStartMs = 0;    // millis() timestamp; valid only if event == Recovered
  unsigned long outageDurationMs = 0; // valid only if event == Recovered
};

// Call every NET_WATCHDOG_CHECK_INTERVAL_MS. Probes with a short raw TCP
// connect to a fixed IP (DNS needs WAN too) and runs the outage/pulse
// decision.
NetWatchdogCheckResult checkInternetAndMaybePulseRelay();

// Hardware page status; doesn't re-probe.
struct NetWatchdogStatus {
  bool settingEnabled = false;
  bool available = false;       // netWatchdogActive()'s value
  bool outageInProgress = false;
  unsigned long outageStartMs = 0; // millis() timestamp; meaningful only if outageInProgress
};
NetWatchdogStatus getNetWatchdogStatus();

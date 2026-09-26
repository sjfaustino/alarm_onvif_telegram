#pragma once
#include <Arduino.h>
#include "config.h" // POWER_MONITOR_PIN_DEFAULT

// Optional mains power monitor (an input, unlike the two relay watchdogs): a
// transformer-driven relay closes its NO contact while 220V is present.
// Alert-only - the board and router are on a UPS.

struct PowerMonitorSettings {
  bool enabled = false;
  int pin = POWER_MONITOR_PIN_DEFAULT;
  // Contact wiring: false = COM->GND (pull-up, closed reads LOW), true =
  // COM->3.3V (pull-down, closed reads HIGH).
  bool activeHigh = false;
};
PowerMonitorSettings loadPowerMonitorSettings();
bool savePowerMonitorSettings(const PowerMonitorSettings& settings);

// Call once from setup(). Seeds the confirmed state without alerting (the boot
// notice reports it) if enabled and the pin is safe and unshared; otherwise
// logs and stays inactive.
void initPowerMonitor();

bool powerMonitorActive();

// Call every POWER_MONITOR_CHECK_INTERVAL_MS, WiFi or not, so debouncing stays
// correct through outages. Returns true once per confirmed change (reading
// stable for POWER_MONITOR_DEBOUNCE_MS); the caller sends the alert.
bool checkPowerStateChanged();

// Hardware page / boot notice status. rawPinHigh is a fresh read so wiring can
// be watched live, without the debounce.
struct PowerMonitorStatus {
  bool settingEnabled = false;
  bool available = false;      // powerMonitorActive()'s value
  bool powerPresent = true;    // last CONFIRMED reading; meaningful only if available
  bool rawPinHigh = false;     // fresh digitalRead() right now; meaningful only if available
};
PowerMonitorStatus getPowerMonitorStatus();

#pragma once
#include <Arduino.h>
#include "config.h" // POWER_MONITOR_PIN_DEFAULT

// Optional 220V mains power monitor - the reverse of net_watchdog.h/
// bridge_watchdog.h: an INPUT, not an output. A relay driven by a
// 220V-to-5V transformer closes its NO contact onto a configured pin
// while mains power is present; losing power de-energizes the relay and
// opens the contact. Purely a sensor - the board and its router are
// themselves on a UPS, so there's nothing to pulse or power-cycle here,
// just an alert at boot and on every confirmed state change. See
// config.h's own comment on this feature for the pin-safety/pin-conflict
// reasoning shared with the other two relay/sensor features.

struct PowerMonitorSettings {
  bool enabled = false;
  int pin = POWER_MONITOR_PIN_DEFAULT;
  // True if the pin reads HIGH when mains power is present. Default
  // false matches the common wiring for a dry NO contact: COM->GND,
  // NO->this pin, pin configured INPUT_PULLUP - contact closed (power
  // present) pulls the pin LOW.
  bool activeHigh = false;
};
PowerMonitorSettings loadPowerMonitorSettings();
bool savePowerMonitorSettings(const PowerMonitorSettings& settings);

// Call once from setup() - sets pinMode(INPUT_PULLUP) and seeds the
// initial CONFIRMED reading (no alert here - too early for WiFi/Telegram;
// the boot message itself reports the seeded state once it's ready, see
// getPowerMonitorStatus()) if enabled AND the configured pin passes
// isReservedOrUnsafePin's check AND doesn't conflict (watchdogPinsConflict)
// with either of the other two relay/sensor features' currently-saved
// pins. Serial-only warning and stays inactive otherwise, same tone as
// the other two peripherals' own init-failure paths.
void initPowerMonitor();

// True only if the setting is enabled AND initPowerMonitor() accepted the
// configured pin - mirrors netWatchdogActive()/bridgeWatchdogActive()'s shape.
bool powerMonitorActive();

// Call on POWER_MONITOR_CHECK_INTERVAL_MS's own cadence (main.cpp's
// loop()) - no WiFi gate needed, a digitalRead needs no network, and the
// debounce state below must keep tracking correctly through a WiFi
// outage so the eventual alert send isn't wrong or duplicated once it
// reconnects. A raw reading that disagrees with the last CONFIRMED state
// must keep disagreeing for POWER_MONITOR_DEBOUNCE_MS before it's
// trusted (relay contact chatter/a brief sag shouldn't flip this on a
// single noisy read). Returns true exactly once per confirmed change -
// the caller reads the new state via getPowerMonitorStatus() and sends
// the Telegram alert. False if !powerMonitorActive(), the reading matches
// the last confirmed state, or a disagreement is still within its
// debounce window.
bool checkPowerStateChanged();

// Status for the dashboard (Hardware > 220V Power page) and the boot
// message - reflects live state, doesn't re-read the pin.
struct PowerMonitorStatus {
  bool settingEnabled = false;
  bool available = false;      // powerMonitorActive()'s value
  bool powerPresent = true;    // last CONFIRMED reading; meaningful only if available
};
PowerMonitorStatus getPowerMonitorStatus();

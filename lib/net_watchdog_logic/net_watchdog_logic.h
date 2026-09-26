#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment

// Pure decision logic for the relay watchdogs, tested natively.

// Pins already used (SD, RTC) or unsafe on the ESP32-S3: strapping pins,
// flash/octal PSRAM (26-37), USB-JTAG and UART0. Needed because relay pins
// come from the dashboard or an imported config.
bool isReservedOrUnsafePin(int pin);

// Unsigned subtraction keeps this correct across millis() wraparound.
bool outageThresholdReached(unsigned long firstFailureMs, unsigned long nowMs, uint32_t thresholdMs);

// Two enabled features on the same GPIO. Checked at each save route and again
// at init (for imported configs).
bool watchdogPinsConflict(bool enabledA, int pinA, bool enabledB, int pinB);

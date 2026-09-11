#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment

// Pure decision logic for the internet-connectivity watchdog (relay-based
// router power-cycle) - zero GPIO/network I/O, so it's unit-testable
// natively (test/test_net_watchdog_logic). src/net_watchdog.cpp owns the
// actual pinMode/digitalWrite/WiFiClient mechanics.

// True if pin is one this project already uses for something else
// (SD_CS/SCK/MISO/MOSI, RTC_SDA/SCL - config.h) or an ESP32-S3 pin that's
// unsafe for general GPIO use: strapping pins (0, 3, 45, 46) that can
// affect boot mode if driven the wrong way at reset, and 26-31, reserved
// for octal PSRAM/flash on this board's qio_opi memory type
// (platformio.ini's own comment on BOARD_HAS_PSRAM/memory_type) - driving
// those is a hard hang or worse, not just a wrong pin. This is the one
// runtime-configurable GPIO pin in this codebase (every other pin here is
// a config.h constant nobody but the firmware author touches), so unlike
// those, this needs to reject a bad dashboard value - or one that arrived
// via a hand-edited/imported NVS record, bypassing the dashboard form
// entirely - before pinMode()/digitalWrite() ever touches it.
bool isReservedOrUnsafePin(int pin);

// True once nowMs - firstFailureMs has reached (or passed) thresholdMs -
// the same plain millis()-delta comparison this project uses everywhere
// else (checkWifiSignal, motion cooldowns, etc.), pulled out here so the
// "is it time to act" decision is unit-testable without real hardware or
// a live WiFi connection. Unsigned subtraction is deliberately relied on
// for millis() wraparound safety - correct even if nowMs has wrapped past
// firstFailureMs, same reasoning as every other due-timestamp check here.
bool outageThresholdReached(unsigned long firstFailureMs, unsigned long nowMs, uint32_t thresholdMs);

// True if two independently-configurable relay watchdogs (net_watchdog.h,
// bridge_watchdog.h) would drive the same physical GPIO pin at once - a
// config mistake with no existing precedent to catch it, since this
// project had exactly zero dashboard-configurable pins before this
// codebase's first one (net_watchdog). Only a conflict when BOTH are
// actually enabled on the same pin; either being disabled, or the pins
// simply differing, is fine. Checked both ways at each watchdog's own
// save route (webserver.cpp) - this alone doesn't know which watchdog is
// "new" - plus once more, defensively, by each watchdog's own init() at
// boot in case a hand-edited/imported NVS record bypassed both routes.
bool watchdogPinsConflict(bool enabledA, int pinA, bool enabledB, int pinB);

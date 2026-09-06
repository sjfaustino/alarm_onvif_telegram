#pragma once
#include <Arduino.h>
#include <ctime>

// Optional external battery-backed RTC (DS3231, I2C) - same "optional
// peripheral, off by default, graceful fallback" shape as sd_store.h's SD
// card support. This board's only clock source otherwise is NTP, which
// only runs once WiFi connects (main.cpp's setupTime()) - a DS3231 lets
// the system clock be seeded with a roughly-correct time immediately at
// boot, before WiFi/NTP have had any chance to run, and is itself kept
// corrected from NTP once a real sync lands. lib/rtc_ds3231 has the pure
// register/BCD math; this module is the actual I2C mechanics.

struct RtcSettings {
  bool enabled = false;
};
RtcSettings loadRtcSettings();
bool saveRtcSettings(const RtcSettings& settings);

// Call once from setup(), BEFORE WiFi.begin()/connectWiFi() - so a synced
// RTC can seed the system clock before NTP has any chance to run. No-op
// (never touches the I2C bus at all) if the persisted setting is disabled.
// Logs which of three outcomes happened (disabled / enabled but no ACK at
// the DS3231's I2C address / found) - Serial-only, no Telegram alert: this
// is a clock-accuracy nicety, not a data-loss risk, unlike SD's own
// later-runtime-failure alerting.
void initRtc();

// True only if the setting is enabled AND the chip actually ACKed at
// initRtc() - mirrors sdActive()'s shape exactly.
bool rtcActive();

// Reads the chip's current time into *out (a UTC struct tm - this
// project's system clock and everything stored on the chip are always
// UTC). False (out left untouched) if !rtcActive() or the I2C read fails.
bool readRtcTime(struct tm* out);

// Writes t (UTC) to the chip. False if !rtcActive() or the I2C write
// fails. Call this right after a successful NTP sync (main.cpp's
// setupTime()) to keep the RTC corrected for the next boot - not meant to
// be called on every loop tick, just once per real sync.
bool writeRtcTime(const struct tm& t);

// Status for the dashboard (Network page) - reflects initRtc()'s outcome,
// doesn't re-probe the hardware itself.
struct RtcStatus {
  bool settingEnabled = false;
  bool available = false; // rtcActive()'s value at the time this was read
};
RtcStatus getRtcStatus();

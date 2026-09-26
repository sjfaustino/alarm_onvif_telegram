#pragma once
#include <Arduino.h>
#include <ctime>

// Optional DS3231 RTC, off by default. Seeds the clock at boot before WiFi/NTP
// and is corrected after each NTP sync. lib/rtc_ds3231 has the register math.

struct RtcSettings {
  bool enabled = false;
};
RtcSettings loadRtcSettings();
bool saveRtcSettings(const RtcSettings& settings);

// Call once from setup(), before WiFi. Doesn't touch I2C if disabled. Failure
// is Serial-only: accuracy nicety, not data loss.
void initRtc();

// Enabled and the chip ACKed at init.
bool rtcActive();

// Reads UTC time. False if inactive, the read fails, or the Oscillator Stop
// Flag says the time can't be trusted.
bool readRtcTime(struct tm* out);

// Writes UTC time after an NTP sync and clears the Oscillator Stop Flag
// (best-effort).
bool writeRtcTime(const struct tm& t);

// Network page status; doesn't re-probe.
struct RtcStatus {
  bool settingEnabled = false;
  bool available = false; // rtcActive()'s value at the time this was read
};
RtcStatus getRtcStatus();

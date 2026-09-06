#include "rtc_store.h"
#include "rtc_ds3231.h"
#include "config.h" // RTC_SDA_PIN/RTC_SCL_PIN/DS3231_I2C_ADDR
#include <Preferences.h>
#include <Wire.h>

static const char* NVS_NAMESPACE = "rtcstore";
static const char* NVS_KEY_ENABLED = "enabled";
static const uint8_t DS3231_REG_SECONDS = 0x00; // first of the 7 contiguous clock registers

static bool g_rtcSettingEnabled = false; // cached at boot, see initRtc()
static bool g_rtcAvailable = false;      // see rtcActive()'s comment

RtcSettings loadRtcSettings() {
  Preferences prefs;
  // Read-write, not read-only - see auth_store.cpp's loadDashboardAuth for why.
  prefs.begin(NVS_NAMESPACE, false);
  RtcSettings settings;
  settings.enabled = prefs.getBool(NVS_KEY_ENABLED, false);
  prefs.end();
  return settings;
}

bool saveRtcSettings(const RtcSettings& settings) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  bool ok = prefs.putBool(NVS_KEY_ENABLED, settings.enabled) > 0;
  prefs.end();
  if (!ok) {
    Serial.println("[rtc_store] ERROR: failed to persist the RTC setting to NVS - it will revert "
                    "to the previous value on the next reboot.");
  }
  return ok;
}

void initRtc() {
  RtcSettings settings = loadRtcSettings();
  g_rtcSettingEnabled = settings.enabled;

  if (!g_rtcSettingEnabled) {
    Serial.println("[rtc_store] External RTC is disabled - system clock relies on NTP only, same as always.");
    return;
  }

  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  Wire.beginTransmission(DS3231_I2C_ADDR);
  uint8_t probeResult = Wire.endTransmission();
  if (probeResult != 0) {
    Serial.println("[rtc_store] RTC is enabled, but no chip ACKed at the configured I2C pins/address "
                    "- check wiring and RTC_SDA_PIN/RTC_SCL_PIN in config.h. Falling back to NTP only.");
    return;
  }

  g_rtcAvailable = true;
  Serial.println("[rtc_store] External RTC found.");
}

bool rtcActive() {
  return g_rtcSettingEnabled && g_rtcAvailable;
}

bool readRtcTime(struct tm* out) {
  if (!rtcActive()) return false;

  Wire.beginTransmission(DS3231_I2C_ADDR);
  Wire.write(DS3231_REG_SECONDS);
  if (Wire.endTransmission(false) != 0) return false; // repeated start, keep the bus for the read below

  if (Wire.requestFrom((int)DS3231_I2C_ADDR, 7) != 7) return false;
  Ds3231Registers regs;
  for (auto& b : regs.bytes) b = (uint8_t)Wire.read();

  *out = decodeDs3231(regs);
  return true;
}

bool writeRtcTime(const struct tm& t) {
  if (!rtcActive()) return false;

  Ds3231Registers regs = encodeDs3231(t);
  Wire.beginTransmission(DS3231_I2C_ADDR);
  Wire.write(DS3231_REG_SECONDS);
  for (uint8_t b : regs.bytes) Wire.write(b);
  return Wire.endTransmission() == 0;
}

RtcStatus getRtcStatus() {
  RtcStatus status;
  status.settingEnabled = g_rtcSettingEnabled;
  status.available = g_rtcAvailable;
  return status;
}

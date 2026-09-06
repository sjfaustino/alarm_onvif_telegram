#include "rtc_store.h"
#include "rtc_ds3231.h"
#include "config.h" // RTC_SDA_PIN/RTC_SCL_PIN/DS3231_I2C_ADDR
#include <Preferences.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char* NVS_NAMESPACE = "rtcstore";
static const char* NVS_KEY_ENABLED = "enabled";
static const uint8_t DS3231_REG_SECONDS = 0x00; // first of the 7 contiguous clock registers

static bool g_rtcSettingEnabled = false; // cached at boot, see initRtc()
static bool g_rtcAvailable = false;      // see rtcActive()'s comment
// Guards every I2C transaction against the DS3231 - readRtcTime() can run
// on a PsychicHttp request task (the Network page reads it directly) at
// the same moment writeRtcTime() runs on loop()'s task (setupTime()'s
// post-NTP-sync writeback, on a WiFi reconnect) - Wire's multi-call
// transaction sequence (beginTransmission/write/endTransmission/
// requestFrom/read) isn't atomic across tasks on its own, so an
// overlapping read and write could otherwise interleave and corrupt each
// other. Same reasoning as sd_store.cpp's g_sdMutex/telegram.cpp's
// g_telegramNetMutex - a shared hardware peripheral touched from more
// than one task needs a lock around each transaction.
static SemaphoreHandle_t g_rtcMutex = xSemaphoreCreateMutex();

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
  xSemaphoreTake(g_rtcMutex, portMAX_DELAY);
  Wire.beginTransmission(DS3231_I2C_ADDR);
  uint8_t probeResult = Wire.endTransmission();
  xSemaphoreGive(g_rtcMutex);
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

  xSemaphoreTake(g_rtcMutex, portMAX_DELAY);
  Wire.beginTransmission(DS3231_I2C_ADDR);
  Wire.write(DS3231_REG_SECONDS);
  if (Wire.endTransmission(false) != 0) { // repeated start, keep the bus for the read below
    xSemaphoreGive(g_rtcMutex);
    return false;
  }

  if (Wire.requestFrom((int)DS3231_I2C_ADDR, 7) != 7) {
    xSemaphoreGive(g_rtcMutex);
    return false;
  }
  Ds3231Registers regs;
  for (auto& b : regs.bytes) b = (uint8_t)Wire.read();
  xSemaphoreGive(g_rtcMutex);

  *out = decodeDs3231(regs);
  return true;
}

bool writeRtcTime(const struct tm& t) {
  if (!rtcActive()) return false;

  Ds3231Registers regs = encodeDs3231(t);
  xSemaphoreTake(g_rtcMutex, portMAX_DELAY);
  Wire.beginTransmission(DS3231_I2C_ADDR);
  Wire.write(DS3231_REG_SECONDS);
  for (uint8_t b : regs.bytes) Wire.write(b);
  bool ok = Wire.endTransmission() == 0;
  xSemaphoreGive(g_rtcMutex);
  return ok;
}

RtcStatus getRtcStatus() {
  RtcStatus status;
  status.settingEnabled = g_rtcSettingEnabled;
  status.available = g_rtcAvailable;
  return status;
}

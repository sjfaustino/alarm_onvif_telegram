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
// Wire transactions aren't atomic across tasks; the Network page (web server)
// can read while loop() writes after an NTP sync.
static SemaphoreHandle_t g_rtcMutex = xSemaphoreCreateMutex();

RtcSettings loadRtcSettings() {
  Preferences prefs;
  // Read-write (see loadDashboardAuth).
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

// Caller holds g_rtcMutex.
static bool readRegisterLocked(uint8_t reg, uint8_t* outByte) {
  Wire.beginTransmission(DS3231_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false; // repeated start, keep the bus for the read below
  if (Wire.requestFrom((int)DS3231_I2C_ADDR, 1) != 1) return false;
  *outByte = (uint8_t)Wire.read();
  return true;
}

// Caller holds g_rtcMutex.
static bool writeRegisterLocked(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(DS3231_I2C_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
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

  // Checked on every read: the chip can lose power (battery swap) at any time.
  uint8_t statusByte = 0;
  bool statusOk = readRegisterLocked(DS3231_STATUS_REG, &statusByte);
  xSemaphoreGive(g_rtcMutex);

  if (!statusOk) return false;
  if (ds3231OscillatorStopped(statusByte)) {
    Serial.println("[rtc_store] WARNING: RTC's oscillator-stop flag is set - its reported time is "
                    "not trustworthy (backup battery dead or missing during a past power loss). "
                    "Treating this read as failed until a real time is written back to it.");
    return false;
  }

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

  // Best-effort clear of the Oscillator Stop Flag (read-modify-write keeps the
  // other bits). If it fails, reads keep distrusting the chip - the safe
  // direction.
  if (ok) {
    uint8_t statusByte = 0;
    if (readRegisterLocked(DS3231_STATUS_REG, &statusByte) && ds3231OscillatorStopped(statusByte)) {
      if (!writeRegisterLocked(DS3231_STATUS_REG, statusByte & (uint8_t)~DS3231_OSF_BIT)) {
        Serial.println("[rtc_store] WARNING: failed to clear the RTC's oscillator-stop flag after "
                        "writing a corrected time.");
      }
    }
  }

  xSemaphoreGive(g_rtcMutex);
  return ok;
}

RtcStatus getRtcStatus() {
  RtcStatus status;
  status.settingEnabled = g_rtcSettingEnabled;
  status.available = g_rtcAvailable;
  return status;
}

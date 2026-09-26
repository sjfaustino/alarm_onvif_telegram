#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <ctime>

// Pure DS3231 register/BCD math, tested natively; rtc_store.cpp does the I2C.

// Raw clock registers 0x00-0x06.
struct Ds3231Registers {
  uint8_t bytes[7] = {0};
};

// UTC struct tm -> registers. Always 24-hour mode; years are 2000-based and
// the century bit is ignored.
Ds3231Registers encodeDs3231(const struct tm& tmStruct);

// Registers -> struct tm (tm_yday/tm_isdst left 0). Masks the 12/24 and AM/PM
// bits, so a chip set to 12-hour mode elsewhere can't give an out-of-range
// hour (though the value may be off).
struct tm decodeDs3231(const Ds3231Registers& regs);

// UTC struct tm -> epoch, since newlib here has no timegm() and mktime() uses
// local time (TZ isn't set yet at boot). Valid for 2000-2099.
time_t timeGmUtc(const struct tm& t);

// Oscillator Stop Flag (status reg bit 7): the oscillator stopped at some
// point, usually a dead backup battery, so the time can't be trusted until
// rewritten. readRtcTime checks it; writeRtcTime clears it.
static const uint8_t DS3231_STATUS_REG = 0x0F;
static const uint8_t DS3231_OSF_BIT = 0x80;

bool ds3231OscillatorStopped(uint8_t statusRegisterByte);

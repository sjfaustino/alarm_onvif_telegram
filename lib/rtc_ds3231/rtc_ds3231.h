#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <ctime>

// Pure DS3231 register/BCD math - zero I2C or hardware I/O, so it's
// unit-testable natively (test/test_rtc_ds3231). rtc_store.h/.cpp own the
// actual Wire.h/I2C glue around this.

// The 7 DS3231 clock registers (0x00-0x06), raw bytes exactly as read from
// or written to the chip over I2C.
struct Ds3231Registers {
  uint8_t bytes[7] = {0};
};

// Encodes tmStruct (a UTC struct tm - this project's system clock is always
// UTC, see setupTime()'s own comment in main.cpp) into the DS3231's BCD
// register layout. Always writes 24-hour mode (clears the hours register's
// 12/24 bit) - this project only ever reads a chip back the same way it
// wrote it, so there's no need to support 12-hour mode at all. Years are
// always treated as 2000+ (tm_year - 100, i.e. tm_year mod 100) - the
// DS3231's century bit is intentionally left untouched/ignored; nothing
// here will ever legitimately run before the year 2000.
Ds3231Registers encodeDs3231(const struct tm& tmStruct);

// Decodes raw registers back into a struct tm (fields tm_sec..tm_year and
// tm_wday populated; tm_yday/tm_isdst left at 0 - callers needing a real
// epoch value should use timegm(), which ignores both) - the inverse of
// encodeDs3231 above. Defensively masks off the hours register's 12/24 and
// AM/PM bits and always interprets the result as 24-hour - tolerates a
// chip that happens to have been set into 12-hour mode by something other
// than this project, without crashing or producing an out-of-range hour,
// though the value read back in that specific case may not be correct
// (this project never itself writes 12-hour mode, so that's an acceptable,
// not a "must handle perfectly," edge case).
struct tm decodeDs3231(const Ds3231Registers& regs);

// Converts a UTC struct tm (tm_sec..tm_year, as decodeDs3231 above
// produces) to a time_t epoch value - a portable replacement for the
// standard `timegm()`, which this platform's libc (ESP32/newlib) doesn't
// provide at all (confirmed at compile time, not merely undocumented).
// Unlike `mktime()`, which interprets its input as LOCAL time via the
// process's current TZ setting, this always treats t as UTC regardless of
// TZ - the only correct choice for rtc_store.cpp's boot-time clock seeding,
// which runs before main.cpp ever sets TZ to anything. Valid for tm_year
// 100-199 (years 2000-2099) - the only range this project's RTC support
// will ever produce (see encodeDs3231/decodeDs3231's own "2000+" comment);
// behavior outside that range is not a contract of this function.
time_t timeGmUtc(const struct tm& t);

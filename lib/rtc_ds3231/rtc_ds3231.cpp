#include "rtc_ds3231.h"

static uint8_t decToBcd(int dec) {
  return (uint8_t)(((dec / 10) << 4) | (dec % 10));
}

static int bcdToDec(uint8_t bcd) {
  return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

Ds3231Registers encodeDs3231(const struct tm& tmStruct) {
  Ds3231Registers regs;
  regs.bytes[0] = decToBcd(tmStruct.tm_sec) & 0x7F;
  regs.bytes[1] = decToBcd(tmStruct.tm_min) & 0x7F;
  // 24-hour mode - bit 6 cleared. decToBcd(0-23) never sets bit 6 on its
  // own (tens digit is at most 2, landing in bits 5-4), but cleared
  // explicitly anyway so the intent reads plainly rather than relying on
  // that being incidentally true.
  regs.bytes[2] = decToBcd(tmStruct.tm_hour) & 0x3F;
  // Day-of-week register wants 1-7, not BCD (a single digit 1-7 is
  // identical either way) - tm_wday is 0-6 (Sunday-Saturday).
  regs.bytes[3] = (uint8_t)((tmStruct.tm_wday % 7) + 1);
  regs.bytes[4] = decToBcd(tmStruct.tm_mday) & 0x3F;
  // Century bit (bit 7) deliberately left clear - see this header's own
  // comment on why years are always treated as 2000+.
  regs.bytes[5] = decToBcd(tmStruct.tm_mon + 1) & 0x1F;
  regs.bytes[6] = decToBcd(tmStruct.tm_year % 100);
  return regs;
}

// Howard Hinnant's well-known "days from civil" algorithm - correct for
// the proleptic Gregorian calendar (including leap years) across any
// year, not just the 2000-2099 range this project actually needs. Returns
// days since 1970-01-01 (the Unix epoch) for the given calendar date.
static long daysFromCivil(long y, unsigned m, unsigned d) {
  y -= (m <= 2) ? 1 : 0;
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   // [0, 146096]
  return era * 146097 + (long)doe - 719468;
}

time_t timeGmUtc(const struct tm& t) {
  long days = daysFromCivil(t.tm_year + 1900, (unsigned)(t.tm_mon + 1), (unsigned)t.tm_mday);
  return (time_t)days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

bool ds3231OscillatorStopped(uint8_t statusRegisterByte) {
  return (statusRegisterByte & DS3231_OSF_BIT) != 0;
}

struct tm decodeDs3231(const Ds3231Registers& regs) {
  struct tm result = {};
  result.tm_sec  = bcdToDec(regs.bytes[0] & 0x7F);
  result.tm_min  = bcdToDec(regs.bytes[1] & 0x7F);
  result.tm_hour = bcdToDec(regs.bytes[2] & 0x3F); // masks off 12/24 + AM/PM bits - see header comment
  result.tm_wday = ((regs.bytes[3] & 0x07) - 1 + 7) % 7;
  result.tm_mday = bcdToDec(regs.bytes[4] & 0x3F);
  result.tm_mon  = bcdToDec(regs.bytes[5] & 0x1F) - 1;
  result.tm_year = bcdToDec(regs.bytes[6]) + 100; // 2000+ - see header comment
  return result;
}

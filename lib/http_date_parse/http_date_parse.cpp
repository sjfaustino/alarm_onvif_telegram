#include "http_date_parse.h"
#include <cctype>

static const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// True if dateHeader[from..from+count) are all ASCII digits - every
// numeric field below is checked this way before toInt(), since
// String::toInt() silently returns 0 for non-numeric input rather than
// signaling failure, which would otherwise let a malformed header parse
// as a plausible-looking (but wrong) midnight-Jan-1900 time instead of
// being rejected outright.
static bool allDigits(const String& s, int from, int count) {
  for (int i = from; i < from + count; i++) {
    if (i >= (int)s.length() || !isdigit((unsigned char)s[i])) return false;
  }
  return true;
}

bool parseHttpDate(const String& dateHeader, struct tm& outTm) {
  // "Sun, 06 Nov 1994 08:49:37 GMT" - IMF-fixdate is always exactly this
  // shape: 3-letter day name, ", ", 2-digit day, " ", 3-letter month,
  // " ", 4-digit year, " ", HH:MM:SS, " GMT". Length and delimiter
  // positions are checked structurally before trusting any field's
  // content - a header that's merely the right length but garbled
  // elsewhere (e.g. "Sun; 06/Nov/1994...") must still be rejected, not
  // misparsed.
  if (dateHeader.length() != 29) return false;
  if (dateHeader[3] != ',' || dateHeader[4] != ' ') return false;
  if (dateHeader[7] != ' ' || dateHeader[11] != ' ' || dateHeader[16] != ' ') return false;
  if (dateHeader[19] != ':' || dateHeader[22] != ':' || dateHeader[25] != ' ') return false;
  if (dateHeader.substring(26) != "GMT") return false;

  if (!allDigits(dateHeader, 5, 2)) return false;
  int day = dateHeader.substring(5, 7).toInt();

  String monthStr = dateHeader.substring(8, 11);
  int month = -1;
  for (int m = 0; m < 12; m++) {
    if (monthStr == kMonths[m]) { month = m; break; }
  }
  if (month < 0) return false;

  if (!allDigits(dateHeader, 12, 4)) return false;
  int year = dateHeader.substring(12, 16).toInt();

  if (!allDigits(dateHeader, 17, 2) || !allDigits(dateHeader, 20, 2) || !allDigits(dateHeader, 23, 2)) return false;
  int hour = dateHeader.substring(17, 19).toInt();
  int minute = dateHeader.substring(20, 22).toInt();
  int second = dateHeader.substring(23, 25).toInt();

  // Loose range checks, not a full calendar/leap-year validator (e.g. day
  // 31 in April would pass here) - this is a fallback clock source, not a
  // date-correctness auditor; timeGmUtc()/settimeofday() tolerate an
  // out-of-calendar struct tm the same way mktime()'s normalization does,
  // and a server sending a nonsensical date is already such an unlikely,
  // low-stakes failure mode that rejecting only the clearly-impossible
  // values (day 0, hour 24, a 3-digit year typo, ...) is enough.
  if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false; // 60 tolerates a leap second
  if (year < 1970) return false; // never legitimately true for a live server's own clock

  outTm = {};
  outTm.tm_mday = day;
  outTm.tm_mon = month;
  outTm.tm_year = year - 1900;
  outTm.tm_hour = hour;
  outTm.tm_min = minute;
  outTm.tm_sec = second;
  return true;
}

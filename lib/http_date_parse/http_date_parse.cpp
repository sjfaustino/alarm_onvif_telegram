#include "http_date_parse.h"
#include <cctype>

static const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// toInt() returns 0 for garbage, so check digits first.
static bool allDigits(const String& s, int from, int count) {
  for (int i = from; i < from + count; i++) {
    if (i >= (int)s.length() || !isdigit((unsigned char)s[i])) return false;
  }
  return true;
}

bool parseHttpDate(const String& dateHeader, struct tm& outTm) {
  // IMF-fixdate is exactly 29 chars: "Sun, 06 Nov 1994 08:49:37 GMT". Check
  // the structure before trusting any field.
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

  // Loose ranges only (April 31 passes); this is a fallback clock.
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

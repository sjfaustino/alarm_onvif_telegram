#include "quiet_hours.h"

bool isWithinQuietHours(int nowMinuteOfDay, int startMinute, int endMinute) {
  if (startMinute == endMinute) return false; // zero-width window - safe default, see header comment
  if (startMinute < endMinute) {
    return nowMinuteOfDay >= startMinute && nowMinuteOfDay < endMinute;
  }
  return nowMinuteOfDay >= startMinute || nowMinuteOfDay < endMinute; // wraps past midnight
}

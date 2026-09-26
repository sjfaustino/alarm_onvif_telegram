#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <ctime>

// Pure HTTP Date header parsing, tested natively. Used by time_sync.cpp's
// router-time fallback, which gives TLS a plausible clock when NTP fails and
// there's no RTC.

// IMF-fixdate only ("Sun, 06 Nov 1994 08:49:37 GMT") - what real servers send;
// the legacy RFC 850/asctime forms aren't supported. On success outTm is UTC
// (tm_wday/yday/isdst left 0), ready for timeGmUtc(); false leaves it
// untouched.
bool parseHttpDate(const String& dateHeader, struct tm& outTm);

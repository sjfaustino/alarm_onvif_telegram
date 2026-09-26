#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment
#include <ctime>

// Pure parsing of an HTTP "Date" response header, split out so it can be
// unit-tested natively (test/test_http_date_parse) without pulling in
// HTTPClient/WiFi, which only exist on-device. time_sync.cpp's
// seedSystemClockFromRouterHttpDate is the actual HTTP-fetch glue around
// this - a fallback clock source for when NTP fails and there's no RTC to
// seed from (see that function's own comment for the full "why" - a
// certificate-pinned TLS handshake with Telegram needs a roughly-correct
// system clock in the first place, which a plain, unauthenticated HTTP
// request to the router's own admin page can bootstrap without that same
// chicken-and-egg problem).

// Parses `dateHeader` as RFC 7231's IMF-fixdate - the only Date format
// virtually every real HTTP server actually emits ("Sun, 06 Nov 1994
// 08:49:37 GMT", always GMT/UTC, always exactly 29 characters). The two
// legacy formats RFC 7231 also grandfathers in (RFC 850 and asctime) are
// deliberately NOT supported - same "good enough for what's actually seen
// in the field, not a general-purpose date parser" scope as this
// project's other hand-rolled parsers (see xml_helpers.h's own comment).
// Returns false (outTm left untouched) if the string doesn't match this
// exact format or any field is out of range - a fallback time source
// failing to parse should never crash or produce a garbage time. On
// success, outTm's fields are UTC (GMT), ready for timeGmUtc()
// (lib/rtc_ds3231) with no timezone adjustment - tm_wday/tm_yday/tm_isdst
// are left at 0, same as decodeDs3231's own contract, since timeGmUtc()
// ignores both anyway.
bool parseHttpDate(const String& dateHeader, struct tm& outTm);

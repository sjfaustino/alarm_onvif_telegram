#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment

// Small pure text-formatting helpers used by both main.cpp (Telegram
// captions/heartbeat) and webserver.cpp (dashboard HTML) - split out here,
// where formatUptime used to be duplicated separately in each, so they
// can't drift apart and can be unit-tested (test/test_format_utils).

// "1d 2h 3m" (days omitted if 0, no seconds).
String formatUptime(unsigned long ms);

// "Xh Ym ago" (reusing formatUptime), or "just now" under a minute -
// formatUptime alone would print "0h 0m ago" for a fresh event, which
// reads like stale data. nowMs is passed in (rather than read from
// millis() internally) so this stays deterministic to test - pass
// millis() at the call site.
String formatElapsedSince(unsigned long eventMs, unsigned long nowMs);

// Escapes &, <, >, " for safe inclusion in HTML text/attribute content.
// Does not escape ' - every attribute this project emits is double-quoted,
// so that's intentionally left alone; start escaping it too if that ever
// changes.
String htmlEscape(const String& s);

// Percent-encodes anything outside the URL-safe unreserved set (RFC 3986:
// alnum, -, _, ., ~) - for putting free text (e.g. a camera name) into a
// query string.
String urlEncode(const String& s);

// Extracts "host[:port]" out of a "scheme://host[:port]/path..." URL, for
// display (the camera list, both Serial and Telegram versions).
String extractHost(const String& url);

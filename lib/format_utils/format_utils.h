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

// Escapes &, <, >, ", and ' (as &#39;) for safe inclusion in HTML text/
// attribute content AND inside a single-quoted JS string embedded in an
// attribute (e.g. onsubmit="return confirm('...')") - see the .cpp's own
// comment on the ' case for the real reflected-XSS hole an unescaped
// single quote opened there. Every attribute this project emits is
// double-quoted, so ' isn't load-bearing for THAT alone - it's escaped
// because of the nested single-quoted-JS-string case, not in spite of it.
String htmlEscape(const String& s);

// Percent-encodes anything outside the URL-safe unreserved set (RFC 3986:
// alnum, -, _, ., ~) - for putting free text (e.g. a camera name) into a
// query string.
String urlEncode(const String& s);

// Extracts "host[:port]" out of a "scheme://host[:port]/path..." URL, for
// display (the camera list, both Serial and Telegram versions).
String extractHost(const String& url);

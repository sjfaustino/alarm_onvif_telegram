#pragma once
#include <Arduino.h> // explicit, not chained - see camera_serialize.h's comment

// Pure formatting helpers shared by Telegram and the dashboard, tested
// natively.

// "1d 2h 3m" (days omitted if 0).
String formatUptime(unsigned long ms);

// "Xh Ym ago", or "just now" under a minute. nowMs passed in for testability.
String formatElapsedSince(unsigned long eventMs, unsigned long nowMs);

// Escapes & < > " and ' (as &#39;) - the quote matters for single-quoted JS
// inside attributes (onsubmit="return confirm('...')"), which was once an XSS
// hole.
String htmlEscape(const String& s);

// RFC 3986 percent-encoding for query-string values.
String urlEncode(const String& s);

// "host[:port]" from a URL, for display.
String extractHost(const String& url);

// Escapes for a single-quoted JS string inside <script> text. Not htmlEscape:
// entities aren't decoded inside <script>, so &#39; would appear literally.
String jsSingleQuoteEscape(const String& s);

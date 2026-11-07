#pragma once
#include <Arduino.h> // explicit: PlatformIO's LDF only scans a lib's own includes

// HTTP Digest authentication (MD5, qop=auth or none), for cameras whose
// snapshot URL refuses Basic auth. Pure and tested natively.

// Lowercase hex MD5 of data.
String md5Hex(const String& data);

// The value for an Authorization header answering `challenge` (a
// WWW-Authenticate header value starting "Digest"), or "" if it isn't a Digest
// challenge this supports. uri is the request path and query, e.g. "/a?b=1";
// cnonce should be fresh random text.
String digestAuthorization(const String& challenge, const String& method, const String& uri, const String& user,
                           const String& pass, const String& cnonce);

// The path and query of a URL ("/" if it has none), for digestAuthorization.
String requestUriFromUrl(const String& url);

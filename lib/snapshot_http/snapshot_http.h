#pragma once
#include <Arduino.h>
#include "http_digest.h"

// The login and body-reading rules of a snapshot fetch, as templates so tests
// can run them against a fake HTTPClient that reproduces its quirks.

struct SnapshotGetResult {
  int code = 0;
  bool usedDigest = false;
  // A camera answered with a new challenge: the caller should remember
  // `challenge`, which is "" if the retry with it failed.
  bool challengeChanged = false;
  String challenge;
};

// An empty challenge sends Basic. Returns 401 without a request if the
// challenge isn't one digestAuthorization supports.
template <class Http, class MakeCnonce>
int snapshotSendGet(Http& http, const String& url, uint16_t timeoutMs, const char* user, const char* pass,
                    const String& challenge, MakeCnonce makeCnonce) {
  // HTTPClient only exposes headers registered with collectHeaders().
  static const char* kCollectedHeaders[] = {"Transfer-Encoding", "WWW-Authenticate"};
  http.begin(url);
  http.setTimeout(timeoutMs);
  http.collectHeaders(kCollectedHeaders, 2);
  if (challenge.length() == 0) {
    http.setAuthorization(user, pass);
    return http.GET();
  }
  String authorization = digestAuthorization(challenge, "GET", requestUriFromUrl(url), user ? user : "",
                                             pass ? pass : "", makeCnonce());
  if (authorization.length() == 0) return 401;
  // After an earlier end() the Basic credentials are still set, and while they
  // are HTTPClient sends them and silently drops an added Authorization header.
  http.setAuthorization("");
  http.addHeader("Authorization", authorization);
  return http.GET();
}

// Some cameras (a Vatilon H46) refuse Basic and challenge for Digest, which
// HTTPClient can't do. cachedChallenge answers the camera's last challenge up
// front, saving the 401 round trip; a 401 gets one retry with the fresh one.
template <class Http, class MakeCnonce>
SnapshotGetResult snapshotGetWithLogin(Http& http, const String& url, uint16_t timeoutMs, const char* user,
                                       const char* pass, const String& cachedChallenge, MakeCnonce makeCnonce) {
  SnapshotGetResult result;
  result.code = snapshotSendGet(http, url, timeoutMs, user, pass, cachedChallenge, makeCnonce);
  result.usedDigest = cachedChallenge.length() > 0;
  if (result.code != 401) return result;

  String challenge = http.header("WWW-Authenticate");
  http.end();
  String scheme = challenge.substring(0, 6);
  scheme.toLowerCase();
  if (scheme != "digest") return result;

  result.code = snapshotSendGet(http, url, timeoutMs, user, pass, challenge, makeCnonce);
  result.usedDigest = true;
  result.challengeChanged = true;
  result.challenge = result.code == 200 ? challenge : String();
  return result;
}

// Bytes to read: exactly Content-Length when given (reading toward the cap
// waits out a stall on keep-alive connections), else up to maxBytes.
inline size_t snapshotReadCap(int contentLength, size_t maxBytes) {
  return contentLength > 0 ? (size_t)contentLength : maxBytes;
}

// The raw-socket reader would corrupt a chunked body with its framing.
inline bool snapshotBodyIsChunked(const String& transferEncoding) {
  String lower = transferEncoding;
  lower.toLowerCase();
  return lower.indexOf("chunked") >= 0;
}

struct SnapshotBodyRead {
  size_t total = 0;
  bool stalled = false;
};

// Reads until `want` bytes, the end of the stream, or stallMs with no progress.
// Conn: connected(), available(), read(buf, n). Clock: nowMs(), sleepMs(ms).
template <class Conn, class Clock>
SnapshotBodyRead readSnapshotBody(Conn& conn, Clock& clock, uint8_t* buf, size_t want, uint32_t stallMs) {
  SnapshotBodyRead result;
  uint32_t lastProgress = clock.nowMs();
  while (conn.connected() && result.total < want) {
    size_t available = conn.available();
    if (available > 0) {
      size_t remaining = want - result.total;
      result.total += conn.read(buf + result.total, available < remaining ? available : remaining);
      lastProgress = clock.nowMs();
    } else if (clock.nowMs() - lastProgress > stallMs) {
      result.stalled = true;
      break;
    } else {
      clock.sleepMs(2);
    }
  }
  return result;
}

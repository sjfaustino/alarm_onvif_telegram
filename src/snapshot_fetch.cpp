#include "snapshot_http.h"
#include "telegram_internal.h"
#include "config.h"
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <esp_heap_caps.h>

// Camera snapshot fetching. The JPEG is fetched once into PSRAM and resent per
// recipient.

// Prefers PSRAM; the internal-RAM fallback is capped at SNAPSHOT_MAX_BYTES,
// since retrying the failed size would likely fail again. Updates `cap`.
static uint8_t* allocateSnapshotBuffer(size_t& cap) {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (buf) return buf;

  if (cap > SNAPSHOT_MAX_BYTES) cap = SNAPSHOT_MAX_BYTES;
  return (uint8_t*)malloc(cap);
}

struct HttpBodyConn {
  HTTPClient& http;
  NetworkClient* stream;
  bool connected() { return http.connected(); }
  size_t available() { return stream->available(); }
  size_t read(uint8_t* buf, size_t n) { return stream->readBytes(buf, n); }
};

struct ArduinoClock {
  uint32_t nowMs() { return millis(); }
  void sleepMs(uint32_t ms) { delay(ms); }
};

static size_t readSomeBytes(HTTPClient& http, NetworkClient* stream, uint8_t* buf, size_t want) {
  HttpBodyConn conn{http, stream};
  ArduinoClock clock;
  SnapshotBodyRead read = readSnapshotBody(conn, clock, buf, want, 5000);
  if (read.stalled) Serial.println("readSomeBytes: stalled, no progress for 5s");
  return read.total;
}

// Raw-socket read (getStreamPtr), for Content-Length or close-delimited bodies
// only: on a chunked response the chunk framing would corrupt the JPEG. Caller
// free()s.
static uint8_t* fetchSnapshotBuffered(HTTPClient& http, size_t& outLen, size_t cap) {
  outLen = 0;
  uint8_t* buf = allocateSnapshotBuffer(cap); // cap may shrink here (internal-RAM fallback) - read that back below
  if (!buf) {
    Serial.println("Snapshot buffer allocation failed.");
    return nullptr;
  }
  size_t total = readSomeBytes(http, http.getStreamPtr(), buf, cap);
  if (total == 0) {
    free(buf);
    return nullptr;
  }
  outLen = total;
  return buf;
}

// Chunked responses, decoded by getString() (whole body, then capped - the
// camera is trusted). Caller free()s.
static uint8_t* fetchSnapshotBufferedChunked(HTTPClient& http, size_t& outLen) {
  outLen = 0;
  String body = http.getString();
  size_t len = body.length();
  if (len == 0) return nullptr;
  if (len > SNAPSHOT_MAX_BYTES_PSRAM) {
    Serial.printf("Chunked snapshot body too large (%u bytes) - discarding.\n", (unsigned)len);
    return nullptr;
  }
  uint8_t* buf = allocateSnapshotBuffer(len);
  if (!buf) {
    Serial.println("Snapshot buffer allocation failed (chunked path).");
    return nullptr;
  }
  memcpy(buf, body.c_str(), len); // length-bounded, not strlen-based - safe for binary content
  outLen = len;
  return buf;
}

// Clears snapshotInFlight on every return path; a missed clear would stall the
// camera's polling forever.
class SnapshotInFlightGuard {
 public:
  explicit SnapshotInFlightGuard(CameraState& state) : st_(state) {
    CameraStateLock lock(st_);
    st_.snapshotInFlight = true;
  }
  ~SnapshotInFlightGuard() {
    CameraStateLock lock(st_);
    st_.snapshotInFlight = false;
  }
  SnapshotInFlightGuard(const SnapshotInFlightGuard&) = delete;
  SnapshotInFlightGuard& operator=(const SnapshotInFlightGuard&) = delete;

 private:
  CameraState& st_;
};

// One snapshot from st.snapshotUri, or nullptr (logged). Caller free()s.
uint8_t* fetchOneSnapshot(const CameraConfig& cfg, CameraState& st, size_t& outLen) {
  outLen = 0;
  SnapshotInFlightGuard snapshotGuard(st); // covers every return path below

  // Also called from loop() (/snap), so copy these under the lock.
  String snapshotUri, cachedChallenge; const char* user; const char* pass;
  {
    CameraStateLock lock(st);
    snapshotUri = st.snapshotUri;
    cachedChallenge = st.snapshotDigestChallenge;
    user = st.user;
    pass = st.pass;
  }
  uint32_t startMs = millis();

  HTTPClient http;
  auto makeCnonce = []() {
    char cnonce[17];
    snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    return String(cnonce);
  };
  SnapshotGetResult got = snapshotGetWithLogin(http, snapshotUri, HTTP_TIMEOUT_MS, user, pass, cachedChallenge, makeCnonce);
  int code = got.code;
  bool usedDigest = got.usedDigest;
  if (got.challengeChanged) {
    CameraStateLock lock(st);
    st.snapshotDigestChallenge = got.challenge;
  }
  uint32_t headersMs = millis() - startMs;
  if (code != 200) {
    Serial.printf("[%s] Snapshot GET failed, HTTP %d\n", cfg.name.c_str(), code);
    http.end();
    return nullptr;
  }

  size_t cap = snapshotReadCap(http.getSize(), SNAPSHOT_MAX_BYTES_PSRAM);
  String transferEncoding = http.header("Transfer-Encoding");

  size_t jpgLen = 0;
  uint8_t* jpg = snapshotBodyIsChunked(transferEncoding) ? fetchSnapshotBufferedChunked(http, jpgLen)
                                                              : fetchSnapshotBuffered(http, jpgLen, cap);
  http.end();
  if (!jpg) {
    Serial.printf("[%s] Snapshot fetch failed.\n", cfg.name.c_str());
    return nullptr;
  }
  // Where a slow fetch spends its time: the camera's answer, or the body.
  Serial.printf("[%s] Snapshot %u bytes in %lums (%s, headers after %lums)\n", cfg.name.c_str(), (unsigned)jpgLen,
                (unsigned long)(millis() - startMs), usedDigest ? "Digest" : "Basic", (unsigned long)headersMs);
  outLen = jpgLen;
  return jpg;
}


#include "telegram_internal.h"
#include "config.h"
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <esp_heap_caps.h>

// Camera -> Telegram. PSRAM is a hard requirement (main.cpp's setup()
// refuses to boot without it): a motion alert can go to more than one
// user, so the JPEG is fetched once and resent per recipient (see
// triggerMotionAlert below). SNAPSHOT_MAX_BYTES only matters as
// allocateSnapshotBuffer's fallback cap if a PSRAM allocation fails.

// Allocates up to `cap` bytes, preferring PSRAM. On failure, falls back to
// internal RAM but capped at SNAPSHOT_MAX_BYTES, not the original
// (possibly much larger) `cap` - internal RAM is scarce here, so retrying
// the exact size that just failed on PSRAM would likely just fail again.
// `cap` is updated in place to whatever was actually allocated.
static uint8_t* allocateSnapshotBuffer(size_t& cap) {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (buf) return buf;

  if (cap > SNAPSHOT_MAX_BYTES) cap = SNAPSHOT_MAX_BYTES;
  return (uint8_t*)malloc(cap);
}

// Reads up to `want` bytes from the camera's HTTP stream into buf, using the
// same "wait for data, bail on stall" pattern as before. Returns bytes
// actually read - only less than `want` if the stream ended or stalled.
static size_t readSomeBytes(HTTPClient& http, NetworkClient* stream, uint8_t* buf, size_t want) {
  size_t total = 0;
  uint32_t stallStart = millis();
  while (http.connected() && total < want) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = min(avail, want - total);
      total += stream->readBytes(buf + total, toRead);
      stallStart = millis();
    } else if (millis() - stallStart > 5000) {
      Serial.println("readSomeBytes: stalled, no progress for 5s");
      break;
    } else {
      delay(2);
    }
  }
  return total;
}

// Buffers a snapshot fully (PSRAM if available, else internal RAM) up to
// `cap` bytes. Caller must free() the returned buffer.
//
// Reads via http.getStreamPtr() - the raw socket, bypassing HTTPClient's
// own response-body decoding. Fine for a Content-Length-known or
// connection-close-delimited body, but must never be used for a
// Transfer-Encoding: chunked response, where these "bytes" would actually
// be raw chunk-size/CRLF framing interleaved with the real data,
// corrupting the JPEG. See fetchSnapshotBufferedChunked below for that
// case - fetchOneSnapshot picks the right one.
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

// Buffers a chunked-transfer-encoded snapshot via HTTPClient::getString(),
// which correctly decodes chunk framing (unlike fetchSnapshotBuffered's
// raw stream reads). Only used when the response actually IS chunked.
// getString() reads the whole body into one String regardless of size, so
// it's capped here rather than bounded during the read - acceptable for a
// camera this project's user configured and trusts, not adversarial
// input. Caller must free() the returned buffer.
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

// RAII guard for CameraState::snapshotInFlight (camera.h) - guarantees the
// flag clears on every exit path out of fetchOneSnapshot below (several
// early returns on failure), same reasoning CameraStateLock/TelegramNetLock
// already apply to their own resources. camera.cpp's cameraTaskFn checks
// this flag before starting its own next poll/renew/resubscribe, so a
// fetch that set the flag true but never cleared it on an overlooked exit
// path would silently wedge that camera's own polling forever - not a
// theoretical concern, exactly the class of bug this whole codebase has
// hunted down repeatedly for other "started but might not reach the
// matching cleanup" resources this session.
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

// Fetches exactly one snapshot from st.snapshotUri, buffered fully (see
// fetchSnapshotBuffered). Returns nullptr (and logs) on any failure - HTTP
// GET failure, or the buffer fetch itself failing. Caller must free() a
// non-null result.
uint8_t* fetchOneSnapshot(const CameraConfig& cfg, CameraState& st, size_t& outLen) {
  outLen = 0;
  SnapshotInFlightGuard snapshotGuard(st); // see its own comment - covers every return path below

  // snapshotUri/user/pass are written by this camera's own task
  // (camera.cpp) but this function is also called from loop()'s task via
  // /snap - copy them out under CameraStateLock rather than reading the
  // live fields directly. See CameraState::stateMutex.
  String snapshotUri; const char* user; const char* pass;
  {
    CameraStateLock lock(st);
    snapshotUri = st.snapshotUri;
    user = st.user;
    pass = st.pass;
  }

  HTTPClient http;
  http.begin(snapshotUri);
  http.setAuthorization(user, pass);
  http.setTimeout(HTTP_TIMEOUT_MS);
  // Registered so http.header() can actually see it after GET() - without
  // collectHeaders(), HTTPClient parses Transfer-Encoding internally for
  // its own use but doesn't expose it through header() at all. See
  // fetchSnapshotBuffered's own comment for why detecting this matters.
  static const char* kCollectedHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(kCollectedHeaders, 1);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[%s] Snapshot GET failed, HTTP %d\n", cfg.name.c_str(), code);
    http.end();
    return nullptr;
  }

  int len = http.getSize();
  // Read exactly `len` bytes when the camera reports Content-Length, so
  // readSomeBytes stops as soon as the file is fully read instead of
  // reading toward the full cap and eating a 5s stall-timeout on a
  // keep-alive connection with no more data to send (confirmed slow enough
  // on one camera - a Reolink cgi-bin - to make the Telegram send that
  // followed fail outright).
  size_t cap = (len > 0) ? (size_t)len : SNAPSHOT_MAX_BYTES_PSRAM;

  String transferEncoding = http.header("Transfer-Encoding");
  transferEncoding.toLowerCase();

  size_t jpgLen = 0;
  uint8_t* jpg = (transferEncoding.indexOf("chunked") >= 0) ? fetchSnapshotBufferedChunked(http, jpgLen)
                                                              : fetchSnapshotBuffered(http, jpgLen, cap);
  http.end();
  if (!jpg) {
    Serial.printf("[%s] Snapshot fetch failed.\n", cfg.name.c_str());
    return nullptr;
  }
  outLen = jpgLen;
  return jpg;
}


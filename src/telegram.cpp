#include "telegram.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ============================================================
// Camera -> Telegram streaming relay
//
// Old approach: fetch the whole JPEG into one malloc'd buffer, THEN open the
// TLS connection to Telegram. That buffer (up to SNAPSHOT_MAX_BYTES, ~100KB)
// sat in RAM at the exact moment mbedTLS needed its own ~16KB+16KB session
// buffers, on a no-PSRAM ESP32-S2 - which is what produced "Memory
// allocation failed" at 54KB nominally free (the free space was fragmented,
// not exhausted).
//
// New approach: as soon as the camera's Content-Length is known, open the
// Telegram TLS connection, send the multipart headers (with the correct
// final Content-Length computed up front), then alternate small reads from
// the camera's plain-HTTP stream with small writes into Telegram's TLS
// stream, STREAM_CHUNK_BYTES at a time. The full JPEG is never resident;
// only one chunk buffer is. This does NOT shrink mbedTLS's own fixed
// session buffers (that's baked into the precompiled core - see config.h),
// but it removes the other large allocation that was competing with them.
//
// Requires the camera to report a real Content-Length (checked by the
// caller); Telegram needs the total multipart body size up front, so a
// chunked/unknown-length source falls back to the old buffer-then-send path.
// ============================================================

// WiFiClientSecure::write() can do a partial write under TLS, especially on
// memory-constrained boards. This loops until every byte is confirmed
// written, the connection drops, or it stalls for 5s with no progress.
static size_t writeAllBytes(WiFiClientSecure& client, const uint8_t* data, size_t len) {
  size_t written = 0;
  uint32_t stallStart = millis();
  while (written < len) {
    if (!client.connected()) {
      Serial.println("writeAllBytes: connection dropped mid-write");
      break;
    }
    size_t n = client.write(data + written, len - written);
    if (n > 0) {
      written += n;
      stallStart = millis();
    } else if (millis() - stallStart > 5000) {
      Serial.println("writeAllBytes: stalled, no progress for 5s");
      break;
    } else {
      delay(2);
    }
  }
  return written;
}

// Reads up to `want` bytes from the camera's HTTP stream into buf, using the
// same "wait for data, bail on stall" pattern as before. Returns bytes
// actually read - only less than `want` if the stream ended or stalled.
static size_t readSomeBytes(HTTPClient& http, WiFiClient* stream, uint8_t* buf, size_t want) {
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

// Fallback for when the camera doesn't report Content-Length (chunked or
// unknown): buffer the whole snapshot first, same as the original code did
// for every request. Caller must free() the returned buffer.
static uint8_t* fetchSnapshotBuffered(HTTPClient& http, size_t& outLen) {
  outLen = 0;
  uint8_t* buf = (uint8_t*)malloc(SNAPSHOT_MAX_BYTES);
  if (!buf) {
    Serial.println("malloc failed for snapshot buffer.");
    return nullptr;
  }
  size_t total = readSomeBytes(http, http.getStreamPtr(), buf, SNAPSHOT_MAX_BYTES);
  if (total == 0) {
    free(buf);
    return nullptr;
  }
  outLen = total;
  return buf;
}

// Shared multipart header/trailer builder + response reader, used by both
// the streamed and buffered send paths so they can't drift apart.
struct TelegramMultipart {
  String boundary, head, tail, requestLine;
  size_t contentLength;
};

static TelegramMultipart buildMultipart(size_t jpgLen, const String& caption) {
  TelegramMultipart m;
  m.boundary = "----ESP32Boundary7MA4YWxk";
  m.head.reserve(160 + caption.length());
  m.head += "--" + m.boundary + "\r\n";
  m.head += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + String(TELEGRAM_CHAT_ID) + "\r\n";
  m.head += "--" + m.boundary + "\r\n";
  m.head += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n";
  m.head += "--" + m.boundary + "\r\n";
  m.head += "Content-Disposition: form-data; name=\"photo\"; filename=\"snap.jpg\"\r\n";
  m.head += "Content-Type: image/jpeg\r\n\r\n";
  m.tail = "\r\n--" + m.boundary + "--\r\n";
  m.contentLength = m.head.length() + jpgLen + m.tail.length();

  m.requestLine.reserve(96 + strlen(TELEGRAM_BOT_TOKEN));
  m.requestLine += "POST /bot" + String(TELEGRAM_BOT_TOKEN) + "/sendPhoto HTTP/1.1\r\n";
  m.requestLine += "Host: api.telegram.org\r\n";
  m.requestLine += "Content-Type: multipart/form-data; boundary=" + m.boundary + "\r\n";
  m.requestLine += "Content-Length: " + String(m.contentLength) + "\r\n";
  m.requestLine += "Connection: close\r\n\r\n";
  return m;
}

static bool readTelegramResponse(WiFiClientSecure& client) {
  uint32_t t0 = millis();
  while (client.connected() && !client.available() && millis() - t0 < 10000) delay(10);
  if (!client.available()) {
    Serial.println("Telegram sendPhoto: no response within timeout.");
    client.stop();
    return false;
  }

  String fullResponse;
  uint32_t readStart = millis();
  while (client.connected() && millis() - readStart < 3000) {
    while (client.available()) { fullResponse += (char)client.read(); readStart = millis(); }
  }
  while (client.available()) fullResponse += (char)client.read();
  client.stop();

  String statusLine = fullResponse.substring(0, fullResponse.indexOf('\n'));
  bool ok = statusLine.indexOf("200") > 0;
  Serial.println(ok ? "Telegram sendPhoto OK" : "Telegram sendPhoto FAILED: " + statusLine);
  return ok;
}

// Streams a JPEG straight from the camera's open HTTP connection into
// Telegram's TLS connection, STREAM_CHUNK_BYTES at a time. jpgLen must be
// the camera's exact reported Content-Length for the bytes about to be read
// from camStream.
static bool sendTelegramPhotoStreamed(HTTPClient& http, WiFiClient* camStream,
                                       size_t jpgLen, const String& caption) {
  if (!camStream || jpgLen == 0) return false;

  Serial.printf("Free heap before Telegram send (streamed): %u bytes (max alloc: %u, jpg: %u bytes)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)jpgLen);

  WiFiClientSecure client;
  client.setInsecure(); // simplification - pin Telegram's CA for production use
  if (!client.connect("api.telegram.org", 443)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("Could not connect to api.telegram.org - TLS/socket error: %s\n", errBuf);
    Serial.printf("Free heap at failure: %u bytes\n", (unsigned)ESP.getFreeHeap());
    return false;
  }

  TelegramMultipart m = buildMultipart(jpgLen, caption);

  size_t sent = 0;
  sent += writeAllBytes(client, (const uint8_t*)m.requestLine.c_str(), m.requestLine.length());
  sent += writeAllBytes(client, (const uint8_t*)m.head.c_str(), m.head.length());

  // The relay loop: read a chunk from the camera, push it straight to
  // Telegram, repeat. Only STREAM_CHUNK_BYTES is ever resident at once -
  // the full JPEG never exists in a single buffer.
  uint8_t chunk[STREAM_CHUNK_BYTES];
  size_t jpgRelayed = 0;
  while (jpgRelayed < jpgLen) {
    size_t want = min((size_t)STREAM_CHUNK_BYTES, jpgLen - jpgRelayed);
    size_t got = readSomeBytes(http, camStream, chunk, want);
    if (got == 0) {
      Serial.printf("Camera stream ended early: got %u of %u bytes\n",
                    (unsigned)jpgRelayed, (unsigned)jpgLen);
      break;
    }
    size_t wroteNow = writeAllBytes(client, chunk, got);
    sent += wroteNow;
    jpgRelayed += got;
    if (wroteNow < got) {
      Serial.println("Telegram write stalled mid-photo - aborting send.");
      break;
    }
  }

  sent += writeAllBytes(client, (const uint8_t*)m.tail.c_str(), m.tail.length());

  size_t expectedTotal = m.requestLine.length() + m.contentLength;
  if (jpgRelayed < jpgLen || sent < expectedTotal) {
    Serial.println("Write incomplete - server will never see the full request.");
    client.stop();
    return false;
  }

  return readTelegramResponse(client);
}

// Fallback path for cameras that don't report Content-Length: sends a
// buffer that's already fully in RAM, same behavior as the original code.
static bool sendTelegramPhotoBuffered(const uint8_t* jpg, size_t jpgLen, const String& caption) {
  if (!jpg || jpgLen == 0) return false;

  Serial.printf("Free heap before Telegram send (buffered fallback): %u bytes (max alloc: %u, jpg: %u bytes)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)jpgLen);

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("Could not connect to api.telegram.org - TLS/socket error: %s\n", errBuf);
    Serial.printf("Free heap at failure: %u bytes\n", (unsigned)ESP.getFreeHeap());
    return false;
  }

  TelegramMultipart m = buildMultipart(jpgLen, caption);

  size_t sent = 0;
  sent += writeAllBytes(client, (const uint8_t*)m.requestLine.c_str(), m.requestLine.length());
  sent += writeAllBytes(client, (const uint8_t*)m.head.c_str(), m.head.length());
  sent += writeAllBytes(client, jpg, jpgLen);
  sent += writeAllBytes(client, (const uint8_t*)m.tail.c_str(), m.tail.length());

  size_t expectedTotal = m.requestLine.length() + m.contentLength;
  if (sent < expectedTotal) {
    Serial.println("Write incomplete - server will never see the full request.");
    client.stop();
    return false;
  }

  return readTelegramResponse(client);
}

static String nowTimestampString() {
  time_t now; time(&now);
  struct tm tmStruct; gmtime_r(&now, &tmStruct);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmStruct);
  return String(buf);
}

void triggerMotionAlert(const CameraConfig& cfg, CameraState& st) {
  uint32_t nowMs = millis();
  if (nowMs - st.lastAlert < ALERT_COOLDOWN_MS) return; // cooling down
  st.lastAlert = nowMs;

  if (st.snapshotUri.length() == 0) {
    Serial.printf("[%s] No snapshot URI available - skipping Telegram send.\n", cfg.name);
    return;
  }

  HTTPClient http;
  http.begin(st.snapshotUri);
  http.setAuthorization(cfg.user, cfg.pass);
  http.setTimeout(HTTP_TIMEOUT_MS);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[%s] Snapshot GET failed, HTTP %d\n", cfg.name, code);
    http.end();
    return;
  }

  String caption = String(cfg.name) + " - " + nowTimestampString();
  int len = http.getSize();
  bool ok = false;

  if (len > 0 && (size_t)len <= SNAPSHOT_MAX_BYTES) {
    // Known size up front -> stream camera bytes straight into the Telegram
    // TLS connection. The JPEG is never fully buffered.
    ok = sendTelegramPhotoStreamed(http, http.getStreamPtr(), (size_t)len, caption);
  } else if (len > 0) {
    Serial.printf("[%s] Snapshot too large for SNAPSHOT_MAX_BYTES cap (%d bytes).\n", cfg.name, len);
  } else {
    // No Content-Length (chunked/unknown) - can't tell Telegram the final
    // body size up front without knowing jpgLen, so fall back to buffering
    // the whole snapshot first, same as the original implementation.
    Serial.printf("[%s] Snapshot has no Content-Length - falling back to buffered send.\n", cfg.name);
    size_t jpgLen = 0;
    uint8_t* jpg = fetchSnapshotBuffered(http, jpgLen);
    if (jpg) {
      ok = sendTelegramPhotoBuffered(jpg, jpgLen, caption);
      free(jpg);
    } else {
      Serial.printf("[%s] Buffered snapshot fetch failed.\n", cfg.name);
    }
  }

  http.end();
  if (!ok) {
    Serial.printf("[%s] Telegram send failed.\n", cfg.name);
  }
}

#include "telegram.h"
#include "telegram_ca.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NetworkClient.h> // HTTPClient::getStreamPtr() returns NetworkClient* on Arduino-ESP32 3.x cores
#include <esp_heap_caps.h>
#include <time.h>
#include <cstring>

bool telegramCAConfigured() {
  // Cheap sanity check that this actually looks like a PEM certificate,
  // not a placeholder/filename/empty string left behind by mistake (see
  // telegram_ca.h history - it once held a file path instead of PEM data,
  // which passed the old "REPLACE_WITH" substring check while still
  // being unparseable).
  return strstr(TELEGRAM_ROOT_CA, "-----BEGIN CERTIFICATE-----") != nullptr &&
         strstr(TELEGRAM_ROOT_CA, "-----END CERTIFICATE-----") != nullptr;
}

// ============================================================
// Camera -> Telegram: two send paths, chosen at RUNTIME
//
// This project has now run on three different boards across development -
// no-PSRAM ESP32-S2, no-PSRAM dual-core ESP32, and this PSRAM-equipped
// ESP32-S3 - so rather than hardcode an assumption about the current board,
// the choice is made at boot by checking ESP.getPsramSize(). Whichever
// board this gets flashed to next, it adapts on its own:
//
//   PSRAM present (this board: 8MB): buffer the whole JPEG in PSRAM via
//   allocateSnapshotBuffer() below, then send it in one shot. With a pool
//   that size, the internal-heap fragmentation problem that motivated
//   streaming in the first place doesn't apply, so there's no reason not
//   to take the simpler buffered path.
//
//   No PSRAM: stream the JPEG from the camera's HTTP connection straight
//   into Telegram's TLS connection, STREAM_CHUNK_BYTES at a time, so the
//   full image is never resident in internal RAM alongside mbedTLS's own
//   fixed ~16KB+16KB session buffers (see config.h's PSRAM note for the
//   original failure this fixed).
// ============================================================

// Allocates `cap` bytes for a snapshot buffer, preferring PSRAM when the
// board has it and falling back to internal RAM otherwise (covers both a
// board with no PSRAM at all and the rare case of a PSRAM allocation
// failing e.g. due to fragmentation of the PSRAM pool itself).
static uint8_t* allocateSnapshotBuffer(size_t cap) {
  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)malloc(cap);
  return buf;
}

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

// Buffers a snapshot fully (into PSRAM if available, else internal RAM -
// see allocateSnapshotBuffer above) up to `cap` bytes. Used both as the
// primary path on PSRAM boards and as the no-Content-Length fallback on
// non-PSRAM boards. Caller must free() the returned buffer.
static uint8_t* fetchSnapshotBuffered(HTTPClient& http, size_t& outLen, size_t cap) {
  outLen = 0;
  uint8_t* buf = allocateSnapshotBuffer(cap);
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
static bool sendTelegramPhotoStreamed(HTTPClient& http, NetworkClient* camStream,
                                       size_t jpgLen, const String& caption) {
  if (!camStream || jpgLen == 0) return false;

  Serial.printf("Free heap before Telegram send (streamed): %u bytes (max alloc: %u, jpg: %u bytes)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)jpgLen);

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  if (!client.connect("api.telegram.org", 443)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("Could not connect to api.telegram.org - TLS/socket error: %s\n", errBuf);
    if (!telegramCAConfigured()) {
      Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
    }
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
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  if (!client.connect("api.telegram.org", 443)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("Could not connect to api.telegram.org - TLS/socket error: %s\n", errBuf);
    if (!telegramCAConfigured()) {
      Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
    }
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

// Sends a plain text message via Telegram's sendMessage endpoint (JSON
// body, not multipart - there's no photo). Shares writeAllBytes and
// readTelegramResponse with the photo-send paths above so all three stay
// consistent about what counts as success.
static String jsonEscape(const String& in) {
  String out; out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

bool sendTelegramMessage(const String& text) {
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  if (!client.connect("api.telegram.org", 443)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("sendTelegramMessage: could not connect - %s\n", errBuf);
    if (!telegramCAConfigured()) {
      Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
    }
    return false;
  }

  String body = "{\"chat_id\":\"" + String(TELEGRAM_CHAT_ID) + "\",\"text\":\"" +
                jsonEscape(text) + "\"}";

  String requestLine;
  requestLine.reserve(96 + strlen(TELEGRAM_BOT_TOKEN));
  requestLine += "POST /bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage HTTP/1.1\r\n";
  requestLine += "Host: api.telegram.org\r\n";
  requestLine += "Content-Type: application/json\r\n";
  requestLine += "Content-Length: " + String(body.length()) + "\r\n";
  requestLine += "Connection: close\r\n\r\n";

  size_t sent = 0;
  sent += writeAllBytes(client, (const uint8_t*)requestLine.c_str(), requestLine.length());
  sent += writeAllBytes(client, (const uint8_t*)body.c_str(), body.length());

  if (sent < requestLine.length() + body.length()) {
    Serial.println("sendTelegramMessage: write incomplete.");
    client.stop();
    return false;
  }

  return readTelegramResponse(client);
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
  http.setAuthorization(st.user, st.pass);
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
  bool psramAvailable = ESP.getPsramSize() > 0;

  if (psramAvailable) {
    // Plenty of headroom on this board - buffer the whole snapshot in
    // PSRAM (regardless of whether Content-Length was even reported) and
    // send it in one shot. No fragmentation concern at this pool size, so
    // the chunked-streaming path below isn't needed here.
    size_t jpgLen = 0;
    uint8_t* jpg = fetchSnapshotBuffered(http, jpgLen, SNAPSHOT_MAX_BYTES_PSRAM);
    if (jpg) {
      ok = sendTelegramPhotoBuffered(jpg, jpgLen, caption);
      free(jpg);
    } else {
      Serial.printf("[%s] Snapshot fetch failed.\n", cfg.name);
    }
  } else if (len > 0 && (size_t)len <= SNAPSHOT_MAX_BYTES) {
    // Known size up front -> stream camera bytes straight into the Telegram
    // TLS connection. The JPEG is never fully buffered in internal RAM.
    ok = sendTelegramPhotoStreamed(http, http.getStreamPtr(), (size_t)len, caption);
  } else if (len > 0) {
    Serial.printf("[%s] Snapshot too large for SNAPSHOT_MAX_BYTES cap (%d bytes).\n", cfg.name, len);
  } else {
    // No Content-Length (chunked/unknown) and no PSRAM to fall back on
    // generously - buffer into internal RAM up to SNAPSHOT_MAX_BYTES, same
    // as the original implementation.
    Serial.printf("[%s] Snapshot has no Content-Length - falling back to buffered send.\n", cfg.name);
    size_t jpgLen = 0;
    uint8_t* jpg = fetchSnapshotBuffered(http, jpgLen, SNAPSHOT_MAX_BYTES);
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

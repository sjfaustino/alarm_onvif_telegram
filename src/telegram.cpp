#include "telegram.h"
#include "telegram_ca.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NetworkClient.h> // HTTPClient::getStreamPtr() returns NetworkClient* on Arduino-ESP32 3.x cores
#include <esp_heap_caps.h>
#include <Preferences.h>
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
  if (!st.alertsEnabled) return; // muted via Telegram - see pollTelegramCommands

  uint32_t nowMs = millis();
  if (st.hasAlerted && nowMs - st.lastAlert < ALERT_COOLDOWN_MS) return; // cooling down
  st.lastAlert = nowMs;
  st.hasAlerted = true;

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
    // PSRAM and send it in one shot. No fragmentation concern at this pool
    // size, so the chunked-streaming path below isn't needed here.
    // Read exactly `len` bytes when the camera reports Content-Length, so
    // readSomeBytes stops as soon as the file is fully read instead of
    // reading toward the full SNAPSHOT_MAX_BYTES_PSRAM cap and then eating
    // its 5s stall-timeout on a keep-alive connection that has no more data
    // to send - that stall was slow enough to make the following Telegram
    // send fail outright on at least one camera (Reolink cgi-bin).
    size_t cap = (len > 0) ? (size_t)len : SNAPSHOT_MAX_BYTES_PSRAM;
    size_t jpgLen = 0;
    uint8_t* jpg = fetchSnapshotBuffered(http, jpgLen, cap);
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

// ============================================================
// Remote on/off control (Telegram commands)
//
// pollTelegramCommands() is called periodically from loop() and does a
// short (timeout=0, not long-poll) getUpdates round trip. Parsing is
// hand-rolled rather than pulling in a JSON library - matches how the ONVIF
// side of this project hand-rolls its XML parsing (see onvif_soap.cpp)
// instead of taking on a dependency for what's a small, predictable
// response shape. lastUpdateId isn't persisted - after a reboot the next
// poll may redeliver a couple of already-applied commands, which is
// harmless since /on and /off are idempotent.
// ============================================================

static const char* ALERT_PREF_NAMESPACE = "camctl";

bool loadAlertEnabledPref(size_t index) {
  Preferences prefs;
  prefs.begin(ALERT_PREF_NAMESPACE, true); // read-only
  char key[8];
  snprintf(key, sizeof(key), "c%u", (unsigned)index);
  bool enabled = prefs.getBool(key, true); // default ON if never set
  prefs.end();
  return enabled;
}

static void saveAlertEnabledPref(size_t index, bool enabled) {
  Preferences prefs;
  prefs.begin(ALERT_PREF_NAMESPACE, false);
  char key[8];
  snprintf(key, sizeof(key), "c%u", (unsigned)index);
  prefs.putBool(key, enabled);
  prefs.end();
}

// Inverse of the jsonEscape() used when building outgoing messages - only
// needs to handle what Telegram could plausibly send back in a text field.
static String jsonUnescape(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\\' && i + 1 < in.length()) {
      char n = in[++i];
      switch (n) {
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case 'n':  out += '\n'; break;
        case 'r':  out += '\r'; break;
        case 't':  out += '\t'; break;
        default:   out += n;    break;
      }
    } else {
      out += c;
    }
  }
  return out;
}

static void handleTelegramCommand(const String& text, const CameraConfig cameras[],
                                   CameraState states[], size_t numCameras) {
  Serial.printf("[Telegram] Command received: \"%s\"\n", text.c_str());

  if (text.equalsIgnoreCase("/status")) {
    String msg = "Camera alert status:\n";
    for (size_t i = 0; i < numCameras; i++) {
      if (!cameras[i].enabled) continue;
      msg += String(cameras[i].name) + ": " + (states[i].alertsEnabled ? "ON" : "OFF") + "\n";
    }
    Serial.println("[Telegram] Replying with camera status.");
    sendTelegramMessage(msg);
    return;
  }

  bool turnOn;
  String cameraName;
  if (text.startsWith("/on ")) { turnOn = true; cameraName = text.substring(4); }
  else if (text.startsWith("/off ")) { turnOn = false; cameraName = text.substring(5); }
  else {
    Serial.println("[Telegram] Unrecognized command - ignored.");
    return;
  }

  cameraName.trim();
  for (size_t i = 0; i < numCameras; i++) {
    if (!cameras[i].enabled) continue;
    if (cameraName.equalsIgnoreCase(cameras[i].name)) {
      states[i].alertsEnabled = turnOn;
      saveAlertEnabledPref(i, turnOn);
      Serial.printf("[%s] Alerts turned %s via Telegram.\n", cameras[i].name, turnOn ? "ON" : "OFF");
      sendTelegramMessage(String(cameras[i].name) + " alerts: " + (turnOn ? "ON" : "OFF"));
      return;
    }
  }
  Serial.printf("[Telegram] /%s target not found or disabled: \"%s\"\n", turnOn ? "on" : "off", cameraName.c_str());
  sendTelegramMessage("Unknown or disabled camera: " + cameraName);
}

void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  static long lastUpdateId = 0;

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("[Telegram] pollTelegramCommands: could not connect - will retry next poll.");
    return; // transient - next poll retries
  }

  String requestLine;
  requestLine.reserve(96 + strlen(TELEGRAM_BOT_TOKEN));
  requestLine += "GET /bot" + String(TELEGRAM_BOT_TOKEN) + "/getUpdates?offset=" +
                  String(lastUpdateId + 1) + "&timeout=0 HTTP/1.1\r\n";
  requestLine += "Host: api.telegram.org\r\n";
  requestLine += "Connection: close\r\n\r\n";
  writeAllBytes(client, (const uint8_t*)requestLine.c_str(), requestLine.length());

  uint32_t t0 = millis();
  while (client.connected() && !client.available() && millis() - t0 < 10000) delay(10);
  if (!client.available()) {
    Serial.println("[Telegram] pollTelegramCommands: no response within timeout.");
    client.stop();
    return;
  }

  String body;
  uint32_t readStart = millis();
  while (client.connected() && millis() - readStart < 3000) {
    while (client.available()) { body += (char)client.read(); readStart = millis(); }
  }
  while (client.available()) body += (char)client.read();
  client.stop();

  int bodyStart = body.indexOf("\r\n\r\n");
  if (bodyStart < 0) {
    Serial.println("[Telegram] pollTelegramCommands: malformed response (no header/body split).");
    return;
  }
  body = body.substring(bodyStart + 4);

  // Each update is a JSON object in the "result" array; scan for
  // "update_id" (always its first key) to find each object's start, then
  // brace-count to find its end - cheaper and more robust than string-
  // searching for keys across the whole response, since nested objects
  // (message/from/chat) can't confuse where one update ends and the next
  // begins.
  int searchPos = 0;
  while (true) {
    int updateIdPos = body.indexOf("\"update_id\":", searchPos);
    if (updateIdPos < 0) break;
    long updateId = body.substring(updateIdPos + 12).toInt();

    int objStart = body.lastIndexOf('{', updateIdPos);
    if (objStart < 0) break;
    int depth = 0, objEnd = -1;
    for (int i = objStart; i < (int)body.length(); i++) {
      if (body[i] == '{') depth++;
      else if (body[i] == '}') { depth--; if (depth == 0) { objEnd = i; break; } }
    }
    if (objEnd < 0) break;

    String block = body.substring(objStart, objEnd + 1);
    searchPos = objEnd + 1;
    if (updateId > lastUpdateId) lastUpdateId = updateId;

    // chat.id, not message.from.id or message.message_id - both of which
    // also match a bare "id": search, so this anchors on "chat" first.
    int chatPos = block.indexOf("\"chat\":");
    if (chatPos < 0) continue;
    int chatIdPos = block.indexOf("\"id\":", chatPos);
    if (chatIdPos < 0) continue;
    // toInt() stops at the first non-numeric character, so this doesn't
    // need to know whether a ',' or '}' ends the value.
    long chatId = block.substring(chatIdPos + 5).toInt();
    if (chatId != String(TELEGRAM_CHAT_ID).toInt()) {
      Serial.printf("[Telegram] Ignored command from unauthorized chat ID %ld\n", chatId);
      continue; // only TELEGRAM_CHAT_ID may command this bot
    }

    int textPos = block.indexOf("\"text\":\"");
    if (textPos < 0) continue;
    textPos += 8;
    String rawText;
    for (int i = textPos; i < (int)block.length(); i++) {
      char c = block[i];
      if (c == '"' && block[i - 1] != '\\') break;
      rawText += c;
    }
    handleTelegramCommand(jsonUnescape(rawText), cameras, states, numCameras);
  }
}

#include "telegram.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// One buffer at a time, capped for a no-PSRAM board. Caller must free().
static uint8_t* fetchSnapshot(const CameraConfig& cfg, CameraState& st, size_t& outLen) {
  outLen = 0;

  HTTPClient http;
  http.begin(st.snapshotUri);
  http.setAuthorization(cfg.user, cfg.pass);
  http.setTimeout(HTTP_TIMEOUT_MS);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[%s] Snapshot GET failed, HTTP %d\n", cfg.name, code);
    http.end();
    return nullptr;
  }

  int len = http.getSize();
  size_t cap = (len > 0) ? (size_t)len : SNAPSHOT_MAX_BYTES;
  if (cap > SNAPSHOT_MAX_BYTES) {
    Serial.printf("[%s] Snapshot too large for SNAPSHOT_MAX_BYTES cap.\n", cfg.name);
    http.end();
    return nullptr;
  }

  uint8_t* buf = (uint8_t*)malloc(cap);
  if (!buf) {
    Serial.printf("[%s] malloc failed for snapshot buffer.\n", cfg.name);
    http.end();
    return nullptr;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t total = 0;
  uint32_t startWait = millis();
  while (http.connected() && total < cap && millis() - startWait < 8000) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = min(avail, cap - total);
      total += stream->readBytes(buf + total, toRead);
      startWait = millis();
    } else {
      delay(2);
    }
  }

  http.end();
  outLen = total;
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

static bool sendTelegramPhoto(uint8_t* jpg, size_t jpgLen, const String& caption) {
  if (!jpg || jpgLen == 0) return false;

  WiFiClientSecure client;
  client.setInsecure(); // simplification - pin Telegram's CA for production use
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("Could not connect to api.telegram.org");
    return false;
  }

  String boundary = "----ESP32Boundary7MA4YWxk";
  String head;
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + String(TELEGRAM_CHAT_ID) + "\r\n";
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n";
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"photo\"; filename=\"snap.jpg\"\r\n";
  head += "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t contentLength = head.length() + jpgLen + tail.length();
  String requestLine = "POST /bot" + String(TELEGRAM_BOT_TOKEN) + "/sendPhoto HTTP/1.1\r\n";
  requestLine += "Host: api.telegram.org\r\n";
  requestLine += "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";
  requestLine += "Content-Length: " + String(contentLength) + "\r\n";
  requestLine += "Connection: close\r\n\r\n";

  size_t sent = 0;
  sent += writeAllBytes(client, (const uint8_t*)requestLine.c_str(), requestLine.length());
  sent += writeAllBytes(client, (const uint8_t*)head.c_str(), head.length());
  sent += writeAllBytes(client, jpg, jpgLen);
  sent += writeAllBytes(client, (const uint8_t*)tail.c_str(), tail.length());

  size_t expectedTotal = requestLine.length() + contentLength;
  if (sent < expectedTotal) {
    Serial.println("Write incomplete - server will never see the full request.");
    client.stop();
    return false;
  }

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

  size_t jpgLen = 0;
  uint8_t* jpg = fetchSnapshot(cfg, st, jpgLen);
  if (!jpg) {
    Serial.printf("[%s] Snapshot fetch failed - skipping Telegram send.\n", cfg.name);
    return;
  }

  String caption = String(cfg.name) + " - " + nowTimestampString();
  sendTelegramPhoto(jpg, jpgLen, caption);
  free(jpg);
}

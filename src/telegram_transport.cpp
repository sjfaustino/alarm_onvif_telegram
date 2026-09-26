#include "telegram.h"
#include "telegram_internal.h"
#include "config.h"
#include "telegram_ca.h"
#include "telegram_users.h"
#include "telegram_i18n.h"
#include "telegram_multipart.h"
#include "telegram_retry_queue.h"
#include "event_log_store.h"
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include <vector>

bool telegramCAConfigured() {
  // Catches a placeholder or filename left in place of a PEM certificate.
  return strstr(TELEGRAM_ROOT_CA, "-----BEGIN CERTIFICATE-----") != nullptr &&
         strstr(TELEGRAM_ROOT_CA, "-----END CERTIFICATE-----") != nullptr;
}

// Allows one TLS session to api.telegram.org at a time, across all tasks.
// mbedTLS state lives in internal RAM; a multi-camera motion burst once pushed
// free heap to ~70KB and failed with "SSL - Memory allocation failed". Camera
// snapshot GETs (plain LAN HTTP) aren't covered.
static SemaphoreHandle_t g_telegramNetMutex = xSemaphoreCreateMutex();

// Bounded wait, unlike CameraStateLock: a camera task stuck here would stop
// renewing its subscription. A timeout counts as a failed send.
TelegramNetLock::TelegramNetLock()
    : held_(xSemaphoreTake(g_telegramNetMutex, pdMS_TO_TICKS(TELEGRAM_NET_MUTEX_TIMEOUT_MS)) == pdTRUE) {}
TelegramNetLock::~TelegramNetLock() { if (held_) xSemaphoreGive(g_telegramNetMutex); }

// Non-blocking check for jobs that should wait for a send to finish rather
// than overlap its memory use (discovery, Test all).
bool telegramSendInProgress() {
  if (xSemaphoreTake(g_telegramNetMutex, 0) == pdTRUE) {
    xSemaphoreGive(g_telegramNetMutex);
    return false;
  }
  return true;
}

// TLS writes can be partial; loop until done, disconnected, or stalled for 5s.
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

  int lineEnd = fullResponse.indexOf('\n');
  if (lineEnd < 0) {
    // No status line: substring(0, -1) would scan the whole body for "200" and
    // could report success falsely.
    Serial.println("Telegram sendPhoto FAILED: no status line in response.");
    return false;
  }
  String statusLine = fullResponse.substring(0, lineEnd);
  bool ok = statusLine.indexOf("200") > 0;
  Serial.println(ok ? "Telegram sendPhoto OK" : "Telegram sendPhoto FAILED: " + statusLine);
  return ok;
}

// One chat; called per recipient with the same PSRAM buffer.
static bool sendTelegramPhotoBuffered(const uint8_t* jpg, size_t jpgLen, const String& caption,
                                       const String& chatId) {
  if (!jpg || jpgLen == 0) return false;

  TelegramNetLock netLock;
  if (!netLock.held()) {
    Serial.println("Telegram sendPhoto: timed out waiting for Telegram send capacity - skipping.");
    return false;
  }

  Serial.printf("Free heap before Telegram send: %u bytes (max alloc: %u, jpg: %u bytes)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(), (unsigned)jpgLen);

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000); // seconds, not ms - unlike every other timeout in this file
  if (!client.connect("api.telegram.org", 443, HTTP_TIMEOUT_MS)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("Could not connect to api.telegram.org - TLS/socket error: %s\n", errBuf);
    if (!telegramCAConfigured()) {
      Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
    }
    Serial.printf("Free heap at failure: %u bytes\n", (unsigned)ESP.getFreeHeap());
    return false;
  }

  TelegramMultipart m = buildMultipart(jpgLen, caption, chatId, TELEGRAM_BOT_TOKEN);

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

// sendDocument for /backup. Single attempt - the admin can just resend.
bool sendTelegramDocumentBuffered(const String& content, const String& filename, const String& caption,
                                          const String& chatId) {
  if (content.length() == 0) return false;

  TelegramNetLock netLock;
  if (!netLock.held()) {
    Serial.println("Telegram sendDocument: timed out waiting for Telegram send capacity - skipping.");
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000); // seconds, not ms - unlike every other timeout in this file
  if (!client.connect("api.telegram.org", 443, HTTP_TIMEOUT_MS)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("Could not connect to api.telegram.org - TLS/socket error: %s\n", errBuf);
    if (!telegramCAConfigured()) {
      Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
    }
    return false;
  }

  TelegramMultipart m = buildDocumentMultipart(content.length(), caption, chatId, TELEGRAM_BOT_TOKEN, filename);

  size_t sent = 0;
  sent += writeAllBytes(client, (const uint8_t*)m.requestLine.c_str(), m.requestLine.length());
  sent += writeAllBytes(client, (const uint8_t*)m.head.c_str(), m.head.length());
  sent += writeAllBytes(client, (const uint8_t*)content.c_str(), content.length());
  sent += writeAllBytes(client, (const uint8_t*)m.tail.c_str(), m.tail.length());

  size_t expectedTotal = m.requestLine.length() + m.contentLength;
  if (sent < expectedTotal) {
    Serial.println("Write incomplete - server will never see the full request.");
    client.stop();
    return false;
  }

  return readTelegramResponse(client);
}

// Retries once on a fresh connection: camera polling sharing the radio has
// stalled large uploads, and a second try often lands in a quieter moment.
bool sendTelegramPhotoWithRetry(const uint8_t* jpg, size_t jpgLen, const String& caption,
                                        const String& chatId) {
  static const int MAX_ATTEMPTS = 2;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (sendTelegramPhotoBuffered(jpg, jpgLen, caption, chatId)) return true;
    if (attempt < MAX_ATTEMPTS) {
      Serial.printf("Telegram photo send to chat %s failed (attempt %d/%d) - retrying.\n",
                    chatId.c_str(), attempt, MAX_ATTEMPTS);
    }
  }
  return false;
}

// Local time per the configured TZ (UTC if none), for display only.
String nowTimestampString() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmStruct);
  return String(buf);
}

// Quiet hours fail open (alerts still send) when the clock was never synced.
bool localClockSynced() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  return tmStruct.tm_year > (2016 - 1900);
}

int currentLocalMinuteOfDay() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  return tmStruct.tm_hour * 60 + tmStruct.tm_min;
}

String formatLocalClockTime(unsigned long dueMs) {
  if (!localClockSynced()) return "";
  // millis() difference is real elapsed time; add it to the current epoch.
  long offsetSec = (long)(dueMs - millis()) / 1000;
  time_t now; time(&now);
  time_t due = now + offsetSec;
  struct tm tmStruct; localtime_r(&due, &tmStruct);
  String hh = String(tmStruct.tm_hour); if (hh.length() < 2) hh = "0" + hh;
  String mm = String(tmStruct.tm_min);  if (mm.length() < 2) mm = "0" + mm;
  return hh + ":" + mm;
}

// JSON-body Bot API calls (sendMessage, answerCallbackQuery). HTTPClient
// handles chunked responses, which Telegram sends.
static bool sendTelegramApiCall(const String& method, JsonDocument& doc) {
  TelegramNetLock netLock;
  if (!netLock.held()) {
    Serial.printf("sendTelegramApiCall(%s): timed out waiting for Telegram send capacity - skipping.\n",
                  method.c_str());
    return false;
  }

  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  client.setHandshakeTimeout(HTTP_TIMEOUT_MS / 1000); // seconds, not ms - see sendTelegramPhotoBuffered's comment

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) + "/" + method;
  if (!http.begin(client, url)) {
    Serial.printf("sendTelegramApiCall(%s): http.begin() failed.\n", method.c_str());
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  bool ok = (code == 200);
  if (!ok) {
    Serial.printf("sendTelegramApiCall(%s): HTTP %d", method.c_str(), code);
    if (code > 0) {
      Serial.printf(" - %s\n", http.getString().c_str());
    } else {
      Serial.printf(" - %s\n", HTTPClient::errorToString(code).c_str());
      if (!telegramCAConfigured()) {
        Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
      }
    }
  }
  http.end();
  return ok;
}

bool sendTelegramMessageTo(const String& chatId, const String& text) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = text;
  return sendTelegramApiCall("sendMessage", doc);
}

bool sendTelegramMessageToChatId(const String& chatId, const String& text) {
  return sendTelegramMessageTo(chatId, text);
}

// One button per row; `buttons` are (label, callback_data). Buttons whose data
// exceeds Telegram's 64-byte limit are skipped and counted in *outSkipped.
bool sendTelegramKeyboardTo(const String& chatId, const String& text,
                                    const std::vector<std::pair<String, String>>& buttons,
                                    size_t* outSkipped) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = text;
  JsonArray rows = doc["reply_markup"]["inline_keyboard"].to<JsonArray>();
  size_t skipped = 0;
  for (auto& b : buttons) {
    if (b.second.length() > 64) {
      Serial.printf("[Telegram] Skipping button \"%s\" - callback_data too long (%u bytes).\n",
                    b.first.c_str(), (unsigned)b.second.length());
      skipped++;
      continue;
    }
    JsonArray row = rows.add<JsonArray>();
    JsonObject btn = row.add<JsonObject>();
    btn["text"] = b.first;
    btn["callback_data"] = b.second;
  }
  if (outSkipped) *outSkipped = skipped;
  return sendTelegramApiCall("sendMessage", doc);
}

// Clears the tapped button's spinner; `text` shows as a toast.
bool answerTelegramCallback(const String& callbackQueryId, const String& text) {
  JsonDocument doc;
  doc["callback_query_id"] = callbackQueryId;
  if (text.length() > 0) doc["text"] = text;
  return sendTelegramApiCall("answerCallbackQuery", doc);
}

// True if at least one recipient got it.
bool sendTelegramMessage(std::function<String(TelegramLang)> compose) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  bool anyRecipient = false;
  bool anyOk = false;
  for (auto& u : users) {
    if (!u.systemMessages) continue;
    anyRecipient = true;
    String text = compose(u.language);
    if (sendTelegramMessageTo(u.chatId, text)) {
      anyOk = true;
    } else {
      // Probably a WAN outage; queue for retry instead of losing it.
      enqueueFailedTelegramMessage(u.chatId, text);
    }
    // Each recipient can wait 45s for the mutex, so several could exceed the
    // 90s loop() watchdog. Harmless on camera tasks, which aren't subscribed.
    esp_task_wdt_reset();
  }
  if (!anyRecipient) {
    Serial.println("sendTelegramMessage: no Telegram user has systemMessages enabled - nothing sent.");
    return false;
  }
  return anyOk;
}


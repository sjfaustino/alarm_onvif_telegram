#include "telegram.h"
#include "telegram_ca.h"
#include "telegram_users.h"
#include "telegram_parse.h"
#include "telegram_multipart.h"
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <NetworkClient.h> // HTTPClient::getStreamPtr() returns NetworkClient* on Arduino-ESP32 3.x cores
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <time.h>
#include <cstring>
#include <vector>

bool telegramCAConfigured() {
  // Sanity check that this actually looks like a PEM certificate, not a
  // placeholder/filename/empty string left behind by mistake.
  return strstr(TELEGRAM_ROOT_CA, "-----BEGIN CERTIFICATE-----") != nullptr &&
         strstr(TELEGRAM_ROOT_CA, "-----END CERTIFICATE-----") != nullptr;
}

// ============================================================
// Camera -> Telegram
//
// PSRAM is a hard requirement (main.cpp's setup() refuses to boot without
// it): a motion alert can go to more than one Telegram user, so the JPEG
// is fetched from the camera once, held in memory, and resent per
// recipient (see triggerMotionAlert below). SNAPSHOT_MAX_BYTES (much
// smaller than SNAPSHOT_MAX_BYTES_PSRAM) survives only as
// allocateSnapshotBuffer's fallback cap if a PSRAM allocation itself fails.
// ============================================================

// Allocates `cap` bytes for a snapshot buffer, preferring PSRAM and
// falling back to internal RAM only if that allocation itself fails.
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

// Sends a buffer that's already fully in RAM to one chat - called once per
// subscribed Telegram user, reusing the same PSRAM buffer (see
// triggerMotionAlert).
static bool sendTelegramPhotoBuffered(const uint8_t* jpg, size_t jpgLen, const String& caption,
                                       const String& chatId) {
  if (!jpg || jpgLen == 0) return false;

  Serial.printf("Free heap before Telegram send: %u bytes (max alloc: %u, jpg: %u bytes)\n",
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

// Retries once (fresh TLS connection) on failure. A failed/stalled write
// (see writeAllBytes' comment) doesn't necessarily mean the network is
// unusable - concurrent camera polling contending for the one WiFi radio
// has been observed to stall a large photo upload until mbedTLS gives up
// and closes the connection; a second attempt often lands in a quieter
// moment.
static bool sendTelegramPhotoWithRetry(const uint8_t* jpg, size_t jpgLen, const String& caption,
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

// localtime_r, not gmtime_r - honors whatever POSIX TZ rule main.cpp's
// setupTime() applied at boot (WifiCredentials::posixTz), or plain UTC if
// none configured. The system clock itself always stays true UTC either
// way - only this *display* value is affected; WS-Security's timestamp
// reads UTC directly via gmtime_r() and is unaffected by TZ.
static String nowTimestampString() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmStruct);
  return String(buf);
}

// Low-level single-recipient send (JSON body, no photo) - the actual HTTPS
// round trip. Used both directly (command replies go back to whoever sent
// the command, not everyone) and by sendTelegramMessage() below (broadcast).
//
// Uses HTTPClient rather than a raw WiFiClientSecure + hand-built request
// line (unlike sendTelegramPhotoBuffered, which streams a multipart body
// HTTPClient has no API for) - HTTPClient correctly handles chunked
// transfer-encoding on the response, which a manual "read until idle, split
// on the first \r\n\r\n" parser doesn't; api.telegram.org is free to send
// a chunked response and has been observed to.
static bool sendTelegramMessageTo(const String& chatId, const String& text) {
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage";
  if (!http.begin(client, url)) {
    Serial.printf("sendTelegramMessageTo(%s): http.begin() failed.\n", chatId.c_str());
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");

  JsonDocument outDoc;
  outDoc["chat_id"] = chatId;
  outDoc["text"] = text;
  String body;
  serializeJson(outDoc, body);

  int code = http.POST(body);
  bool ok = (code == 200);
  if (!ok) {
    Serial.printf("sendTelegramMessageTo(%s): HTTP %d", chatId.c_str(), code);
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

// Broadcasts to every Telegram user with systemMessages enabled - used for
// the heartbeat, the boot-online notice, and the "no credentials" fatal
// alert. Returns true if it reached at least one recipient.
bool sendTelegramMessage(const String& text) {
  std::vector<TelegramUser> users = loadTelegramUsers();
  bool anyRecipient = false;
  bool anyOk = false;
  for (auto& u : users) {
    if (!u.systemMessages) continue;
    anyRecipient = true;
    if (sendTelegramMessageTo(u.chatId, text)) anyOk = true;
  }
  if (!anyRecipient) {
    Serial.println("sendTelegramMessage: no Telegram user has systemMessages enabled - nothing sent.");
    return false;
  }
  return anyOk;
}

// Fetches exactly one snapshot from st.snapshotUri, buffered fully (see
// fetchSnapshotBuffered). Returns nullptr (and logs) on any failure - HTTP
// GET failure, or the buffer fetch itself failing. Caller must free() a
// non-null result.
static uint8_t* fetchOneSnapshot(const CameraConfig& cfg, CameraState& st, size_t& outLen) {
  outLen = 0;

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

  size_t jpgLen = 0;
  uint8_t* jpg = fetchSnapshotBuffered(http, jpgLen, cap);
  http.end();
  if (!jpg) {
    Serial.printf("[%s] Snapshot fetch failed.\n", cfg.name.c_str());
    return nullptr;
  }
  outLen = jpgLen;
  return jpg;
}

void triggerMotionAlert(const CameraConfig& cfg, CameraState& st) {
  // alertsEnabled is written by loop()'s task (pollTelegramCommands'
  // /on//off), this function runs on the camera's own task - cross-task
  // read, needs CameraStateLock. See CameraState::stateMutex.
  bool alertsEnabled;
  { CameraStateLock lock(st); alertsEnabled = st.alertsEnabled; }
  if (!alertsEnabled) return; // muted via Telegram - see pollTelegramCommands

  // hasAlerted/lastAlert are written only here, always from this same
  // task, so this self-read needs no lock - only cross-task readers
  // (webserver.cpp) do.
  uint32_t nowMs = millis();
  if (st.hasAlerted && nowMs - st.lastAlert < cfg.alertCooldownMs) return; // cooling down

  std::vector<String> recipients;
  for (auto& u : loadTelegramUsers()) {
    if (telegramUserWantsCamera(u, cfg.name)) recipients.push_back(u.chatId);
  }
  if (recipients.empty()) {
    Serial.printf("[%s] No Telegram user is subscribed to this camera - skipping send.\n", cfg.name.c_str());
    return;
  }

  // snapshotUri is written only by this camera's own task too (camera.cpp),
  // same-task self-read, no lock needed here either.
  if (st.snapshotUri.length() == 0) {
    Serial.printf("[%s] No snapshot URI available - skipping Telegram send.\n", cfg.name.c_str());
    return;
  }

  // Cooldown is only spent once a send is actually attempted (past this
  // point) - marking it earlier would let a camera with no subscribers, or
  // an unresolved snapshot URI, silently burn every motion event's cooldown
  // doing nothing. A failure past this point still spends it, on purpose,
  // to stop sustained motion from retry-storming a misbehaving camera.
  { CameraStateLock lock(st); st.lastAlert = nowMs; st.hasAlerted = true; }

  unsigned int shots = (cfg.snapshotBurstCount > 0) ? cfg.snapshotBurstCount : 1;

  // Each shot is its own fetch (re-fetching is what makes consecutive
  // shots differ) and its own fan-out to every recipient - re-fetching per
  // recipient instead would hammer cameras whose HTTP stacks only tolerate
  // 1-2 connections. No artificial delay between shots: the fetch and
  // upload each take real time on their own, already enough spacing.
  for (unsigned int i = 0; i < shots; i++) {
    size_t jpgLen = 0;
    uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
    if (!jpg) continue; // one bad shot in a burst shouldn't abort the rest

    String caption = cfg.name + " - " + nowTimestampString();
    if (shots > 1) caption += " (" + String(i + 1) + "/" + String(shots) + ")";

    for (auto& chatId : recipients) {
      if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, chatId)) {
        Serial.printf("[%s] Telegram send to chat %s failed.\n", cfg.name.c_str(), chatId.c_str());
      }
    }
    free(jpg);
  }
}

void checkCameraOnlineStatus(const CameraConfig& cfg, CameraState& st) {
  bool offlineNow = (millis() - st.lastContactMs) >= cfg.offlineThresholdMs;
  // isOffline is written only here, always from this camera's own task, so
  // this self-read needs no lock - only the write below does, since
  // webserver.cpp/main.cpp read it from other tasks.
  if (offlineNow == st.isOffline) return; // no state change - most calls hit this

  { CameraStateLock lock(st); st.isOffline = offlineNow; }
  if (offlineNow) {
    Serial.printf("[%s] OFFLINE - no response for over %lus.\n", cfg.name.c_str(), cfg.offlineThresholdMs / 1000UL);
    sendTelegramMessage("\xE2\x9A\xA0\xEF\xB8\x8F " + cfg.name + " is OFFLINE - no response for over " +
                         String(cfg.offlineThresholdMs / 60000UL) + " minute(s).");
  } else {
    Serial.printf("[%s] Back ONLINE.\n", cfg.name.c_str());
    sendTelegramMessage("\xE2\x9C\x85 " + cfg.name + " is back ONLINE.");
  }
}

// ============================================================
// Remote on/off control (Telegram commands)
//
// pollTelegramCommands() is called periodically from loop() and does a
// short (timeout=0, not long-poll) getUpdates round trip. lastUpdateId
// isn't persisted - after a reboot the next poll may redeliver a couple of
// already-applied commands, harmless since /on and /off are idempotent.
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

// Fetches a fresh snapshot from cfg/st right now and sends it only to
// chatId (whoever asked) - unlike triggerMotionAlert, this is an explicit
// one-off request, not a motion alert, so it ignores st.alertsEnabled and
// doesn't touch st.lastAlert/hasAlerted or spend the alert cooldown.
static void sendOnDemandSnapshot(const CameraConfig& cfg, CameraState& st, const String& chatId) {
  // This runs on loop()'s task, snapshotUri is written by the camera's own
  // task - cross-task read, needs CameraStateLock. See CameraState::stateMutex.
  bool hasSnapshotUri;
  { CameraStateLock lock(st); hasSnapshotUri = st.snapshotUri.length() > 0; }
  if (!hasSnapshotUri) {
    sendTelegramMessageTo(chatId, cfg.name + ": no snapshot URI available yet.");
    return;
  }

  size_t jpgLen = 0;
  uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
  if (!jpg) {
    sendTelegramMessageTo(chatId, cfg.name + ": snapshot fetch failed - see Serial log.");
    return;
  }

  String caption = cfg.name + " - " + nowTimestampString();
  if (!sendTelegramPhotoWithRetry(jpg, jpgLen, caption, chatId)) {
    Serial.printf("[%s] On-demand snapshot send to chat %s failed.\n", cfg.name.c_str(), chatId.c_str());
  }
  free(jpg);
}

static void handleTelegramCommand(const TelegramUser& sender, const String& text, const CameraConfig cameras[],
                                   CameraState states[], size_t numCameras) {
  Serial.printf("[Telegram] Command from chat %s: \"%s\"\n", sender.chatId.c_str(), text.c_str());

  if (text.equalsIgnoreCase("/status")) {
    if (!sender.canCommand) {
      sendTelegramMessageTo(sender.chatId, "You're not authorized to use /status.");
      return;
    }
    // alertsEnabled is only ever written from this same task (below, and
    // main.cpp's boot-time restore), so this self-read needs no lock.
    String msg = "Camera alert status:\n";
    for (size_t i = 0; i < numCameras; i++) {
      if (!cameras[i].enabled) continue;
      msg += String(cameras[i].name) + ": " + (states[i].alertsEnabled ? "ON" : "OFF") + "\n";
    }
    Serial.println("[Telegram] Replying with camera status.");
    sendTelegramMessageTo(sender.chatId, msg);
    return;
  }

  String lowerText = text;
  lowerText.toLowerCase();

  enum class Cmd { On, Off, Snap };
  Cmd cmd;
  String cameraName;
  if (lowerText.startsWith("/on ")) { cmd = Cmd::On; cameraName = text.substring(4); }
  else if (lowerText.startsWith("/off ")) { cmd = Cmd::Off; cameraName = text.substring(5); }
  else if (lowerText.startsWith("/snap ")) { cmd = Cmd::Snap; cameraName = text.substring(6); }
  else {
    Serial.println("[Telegram] Unrecognized command - ignored.");
    return;
  }

  const char* verb = (cmd == Cmd::On) ? "on" : (cmd == Cmd::Off) ? "off" : "snap";
  // canSnap is independent of canCommand - see TelegramUser's comment for
  // why (different kind of trust: pulling a live photo vs. toggling alerts).
  bool authorized = (cmd == Cmd::Snap) ? sender.canSnap : sender.canCommand;
  if (!authorized) {
    Serial.printf("[Telegram] Chat %s not authorized for /%s.\n", sender.chatId.c_str(), verb);
    sendTelegramMessageTo(sender.chatId, "You're not authorized to use /" + String(verb) + ".");
    return;
  }

  cameraName.trim();

  // Matched by prefix ("D01" matches "D01-FDir") - ambiguous matches get
  // nothing applied and a reply listing what matched, rather than guessing.
  std::vector<size_t> matches = matchCamerasByPrefix(cameras, numCameras, cameraName);

  if (matches.size() > 1) {
    String list;
    for (size_t idx : matches) { if (list.length() > 0) list += ", "; list += cameras[idx].name; }
    Serial.printf("[Telegram] /%s target \"%s\" is ambiguous: %s\n", verb, cameraName.c_str(), list.c_str());
    sendTelegramMessageTo(sender.chatId, "\"" + cameraName + "\" matches more than one camera: " + list +
                                          " - be more specific.");
    return;
  }

  if (matches.empty()) {
    Serial.printf("[Telegram] /%s target not found or disabled: \"%s\"\n", verb, cameraName.c_str());
    sendTelegramMessageTo(sender.chatId, "Unknown or disabled camera: " + cameraName);
    return;
  }

  size_t i = matches[0];

  if (cmd == Cmd::Snap) {
    Serial.printf("[%s] On-demand snapshot requested via Telegram.\n", cameras[i].name.c_str());
    sendOnDemandSnapshot(cameras[i], states[i], sender.chatId);
    return;
  }

  bool turnOn = (cmd == Cmd::On);
  { CameraStateLock lock(states[i]); states[i].alertsEnabled = turnOn; } // read cross-task by camera.cpp/webserver.cpp
  saveAlertEnabledPref(i, turnOn);
  Serial.printf("[%s] Alerts turned %s via Telegram.\n", cameras[i].name.c_str(), turnOn ? "ON" : "OFF");
  sendTelegramMessageTo(sender.chatId, String(cameras[i].name) + " alerts: " + (turnOn ? "ON" : "OFF"));
}

void pollTelegramCommands(const CameraConfig cameras[], CameraState states[], size_t numCameras) {
  static long lastUpdateId = 0;

  // HTTPClient rather than a raw socket + hand-rolled "\r\n\r\n" body split
  // (see sendTelegramMessageTo's comment) - that split silently mis-parses
  // if Telegram ever sends a chunked response, since it doesn't decode
  // chunk-size markers before handing the body to parseTelegramUpdates.
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(TELEGRAM_BOT_TOKEN) +
               "/getUpdates?offset=" + String(lastUpdateId + 1) + "&timeout=0";
  if (!http.begin(client, url)) {
    Serial.println("[Telegram] pollTelegramCommands: http.begin() failed.");
    return;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);

  int code = http.GET();
  if (code != 200) {
    String detail = (code > 0) ? String("") : (" - " + HTTPClient::errorToString(code));
    Serial.printf("[Telegram] pollTelegramCommands: HTTP %d%s\n", code, detail.c_str());
    http.end();
    return; // transient - next poll retries
  }
  String body = http.getString();
  http.end();

  String parseError;
  std::vector<TelegramUpdate> updates = parseTelegramUpdates(body, &parseError);
  if (parseError.length() > 0) {
    Serial.printf("[Telegram] pollTelegramCommands: %s\n", parseError.c_str());
  }

  std::vector<TelegramUser> users = loadTelegramUsers();
  for (auto& upd : updates) {
    if (upd.updateId > lastUpdateId) lastUpdateId = upd.updateId;
    if (!upd.hasChatId) continue; // no message on this update (edited_message, channel_post, ...)

    const TelegramUser* sender = nullptr;
    for (auto& u : users) {
      if (chatIdMatches(u.chatId, upd.chatId)) { sender = &u; break; }
    }
    // canCommand and canSnap are independent permissions (see TelegramUser)
    // - a sender needs at least one of them to reach handleTelegramCommand
    // at all; which specific commands that actually unlocks is decided
    // there, per-command.
    if (!sender || !(sender->canCommand || sender->canSnap)) {
      Serial.printf("[Telegram] Ignored command from chat ID %lld (%s)\n", (long long)upd.chatId,
                    sender ? "not authorized to send commands" : "unknown chat");
      continue;
    }

    if (upd.text.length() == 0) continue; // e.g. a sticker or photo with no caption - nothing to act on
    handleTelegramCommand(*sender, upd.text, cameras, states, numCameras);
  }
}

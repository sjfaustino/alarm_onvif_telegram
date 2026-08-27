#include "telegram.h"
#include "telegram_ca.h"
#include "telegram_users.h"
#include "telegram_parse.h"
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
  // Cheap sanity check that this actually looks like a PEM certificate,
  // not a placeholder/filename/empty string left behind by mistake (see
  // telegram_ca.h history - it once held a file path instead of PEM data,
  // which passed the old "REPLACE_WITH" substring check while still
  // being unparseable).
  return strstr(TELEGRAM_ROOT_CA, "-----BEGIN CERTIFICATE-----") != nullptr &&
         strstr(TELEGRAM_ROOT_CA, "-----END CERTIFICATE-----") != nullptr;
}

// ============================================================
// Camera -> Telegram
//
// PSRAM is a hard requirement now (main.cpp's setup() refuses to boot
// without it), because a motion alert can go to more than one Telegram
// user: the JPEG has to be fetched from the camera once, held in memory,
// and resent per recipient (see triggerMotionAlert below). This project did
// run on a couple of earlier no-PSRAM boards, with a streamed send path
// that relayed camera bytes straight into Telegram's TLS connection to
// avoid ever holding the whole JPEG in RAM - that path is gone, since
// streaming consumes the camera's response as it goes and could only ever
// have served a single recipient. SNAPSHOT_MAX_BYTES (much smaller than
// SNAPSHOT_MAX_BYTES_PSRAM) survives only as allocateSnapshotBuffer's
// fallback cap for the rare case a PSRAM allocation itself fails.
// ============================================================

// Allocates `cap` bytes for a snapshot buffer, preferring PSRAM (always
// available - see above) and falling back to internal RAM only if that
// specific allocation fails, e.g. due to PSRAM pool fragmentation.
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

static TelegramMultipart buildMultipart(size_t jpgLen, const String& caption, const String& chatId) {
  TelegramMultipart m;
  m.boundary = "----ESP32Boundary7MA4YWxk";
  m.head.reserve(160 + caption.length());
  m.head += "--" + m.boundary + "\r\n";
  m.head += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + chatId + "\r\n";
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

// Sends a buffer that's already fully in RAM to one chat. Used for every
// recipient of a motion alert - the JPEG is fetched from the camera once
// and this is called once per subscribed Telegram user, reusing the same
// PSRAM buffer (see triggerMotionAlert). This used to have a sibling
// sendTelegramPhotoStreamed() that relayed camera bytes straight into
// Telegram's TLS connection without ever buffering the whole JPEG - dropped
// because streaming consumes the camera's HTTP response as it goes, so it
// can only ever serve a single recipient; multiple Telegram users need the
// JPEG held in memory to resend, which is exactly why PSRAM is now
// mandatory rather than just preferred (see this file's top comment).
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

  TelegramMultipart m = buildMultipart(jpgLen, caption, chatId);

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

// localtime_r, not gmtime_r - honors whatever POSIX TZ rule main.cpp's
// setupTime() applied at boot (see WifiCredentials::posixTz), computing
// DST-aware local time automatically. Falls back to plain UTC if no TZ was
// configured (today's behavior, unchanged). The system clock itself always
// stays true UTC either way - only this *display* value is affected;
// onvif_soap.cpp's WS-Security timestamps read UTC directly via gmtime_r()
// and are unaffected by TZ.
static String nowTimestampString() {
  time_t now; time(&now);
  struct tm tmStruct; localtime_r(&now, &tmStruct);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmStruct);
  return String(buf);
}

// Sends a plain text message via Telegram's sendMessage endpoint (JSON
// body, not multipart - there's no photo). Shares writeAllBytes and
// readTelegramResponse with the photo-send paths above so all three stay
// consistent about what counts as success.

// Low-level single-recipient send - the actual HTTPS round trip. Used both
// directly (command replies, which must go back to whoever sent the
// command, not everyone) and by sendTelegramMessage() below (broadcast to
// every "system messages" user).
static bool sendTelegramMessageTo(const String& chatId, const String& text) {
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA); // see telegram_ca.h if this needs refreshing
  if (!client.connect("api.telegram.org", 443)) {
    char errBuf[128];
    client.lastError(errBuf, sizeof(errBuf));
    Serial.printf("sendTelegramMessageTo(%s): could not connect - %s\n", chatId.c_str(), errBuf);
    if (!telegramCAConfigured()) {
      Serial.println("  ^ TELEGRAM_ROOT_CA in telegram_ca.h is still the placeholder - fill it in.");
    }
    return false;
  }

  // ArduinoJson handles the escaping (quotes, backslashes, control chars,
  // unicode) instead of a hand-rolled jsonEscape() - see telegram_parse.cpp
  // for the equivalent on the receiving side (parseTelegramUpdates).
  JsonDocument outDoc;
  outDoc["chat_id"] = chatId;
  outDoc["text"] = text;
  String body;
  serializeJson(outDoc, body);

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
    Serial.printf("sendTelegramMessageTo(%s): write incomplete.\n", chatId.c_str());
    client.stop();
    return false;
  }

  return readTelegramResponse(client);
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

  HTTPClient http;
  http.begin(st.snapshotUri);
  http.setAuthorization(st.user, st.pass);
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
  if (!st.alertsEnabled) return; // muted via Telegram - see pollTelegramCommands

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

  if (st.snapshotUri.length() == 0) {
    Serial.printf("[%s] No snapshot URI available - skipping Telegram send.\n", cfg.name.c_str());
    return;
  }

  // Only past this point is a send actually attempted, so only past this
  // point is the cooldown worth spending - marking it any earlier meant a
  // camera with no subscribers, or one whose snapshot URI never resolved,
  // silently "used up" every motion event's cooldown window doing nothing:
  // the dashboard's Last Alert column would show a recent timestamp despite
  // nothing ever being sent, and fixing the underlying config issue could
  // still take a while to produce the next real alert. A failure past this
  // point (HTTP GET, buffer fetch, Telegram send) still spends the
  // cooldown, on purpose - that's what stops sustained motion from
  // retry-storming a misbehaving camera every PULL_INTERVAL_MS.
  st.lastAlert = nowMs;
  st.hasAlerted = true;

  unsigned int shots = (cfg.snapshotBurstCount > 0) ? cfg.snapshotBurstCount : 1;

  // Each shot is its own fetch (a snapshot URI just returns "the current
  // frame", so re-fetching is what makes consecutive shots actually differ)
  // and its own fan-out to every recipient, same as the single-shot path
  // always did - re-fetching per recipient instead would hammer cameras
  // whose embedded HTTP stacks only tolerate 1-2 connections (see the
  // boot-stagger comment in main.cpp). No artificial delay between shots -
  // the camera fetch and the Telegram upload each take a real amount of
  // time on their own, which is already enough spacing between frames.
  for (unsigned int i = 0; i < shots; i++) {
    size_t jpgLen = 0;
    uint8_t* jpg = fetchOneSnapshot(cfg, st, jpgLen);
    if (!jpg) continue; // one bad shot in a burst shouldn't abort the rest

    String caption = cfg.name + " - " + nowTimestampString();
    if (shots > 1) caption += " (" + String(i + 1) + "/" + String(shots) + ")";

    for (auto& chatId : recipients) {
      if (!sendTelegramPhotoBuffered(jpg, jpgLen, caption, chatId)) {
        Serial.printf("[%s] Telegram send to chat %s failed.\n", cfg.name.c_str(), chatId.c_str());
      }
    }
    free(jpg);
  }
}

void checkCameraOnlineStatus(const CameraConfig& cfg, CameraState& st) {
  bool offlineNow = (millis() - st.lastContactMs) >= cfg.offlineThresholdMs;
  if (offlineNow == st.isOffline) return; // no state change - most calls hit this

  st.isOffline = offlineNow;
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

// jsonUnescape() itself now lives in telegram_parse.h/cpp.

// Fetches a fresh snapshot from cfg/st right now and sends it only to
// chatId (whoever asked) - unlike triggerMotionAlert, this is an explicit
// one-off request, not a motion alert, so it ignores st.alertsEnabled and
// doesn't touch st.lastAlert/hasAlerted or spend the alert cooldown.
static void sendOnDemandSnapshot(const CameraConfig& cfg, CameraState& st, const String& chatId) {
  if (st.snapshotUri.length() == 0) {
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
  if (!sendTelegramPhotoBuffered(jpg, jpgLen, caption, chatId)) {
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

  // Matched by prefix ("D01" matches "D01-FDir"), not just exact name, so
  // you don't have to type the full camera name - an exact match (name
  // equals the typed text) still works too, since a string always starts
  // with itself. If the prefix is ambiguous (matches more than one enabled
  // camera), nothing is applied and the reply lists what matched, rather
  // than guessing which camera was meant. See test/test_telegram_parse for
  // the matching behavior itself.
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
  states[i].alertsEnabled = turnOn;
  saveAlertEnabledPref(i, turnOn);
  Serial.printf("[%s] Alerts turned %s via Telegram.\n", cameras[i].name.c_str(), turnOn ? "ON" : "OFF");
  sendTelegramMessageTo(sender.chatId, String(cameras[i].name) + " alerts: " + (turnOn ? "ON" : "OFF"));
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
      if (u.chatId.toInt() == upd.chatId) { sender = &u; break; }
    }
    // canCommand and canSnap are independent permissions (see TelegramUser)
    // - a sender needs at least one of them to reach handleTelegramCommand
    // at all; which specific commands that actually unlocks is decided
    // there, per-command.
    if (!sender || !(sender->canCommand || sender->canSnap)) {
      Serial.printf("[Telegram] Ignored command from chat ID %ld (%s)\n", upd.chatId,
                    sender ? "not authorized to send commands" : "unknown chat");
      continue;
    }

    if (upd.text.length() == 0) continue; // e.g. a sticker or photo with no caption - nothing to act on
    handleTelegramCommand(*sender, upd.text, cameras, states, numCameras);
  }
}

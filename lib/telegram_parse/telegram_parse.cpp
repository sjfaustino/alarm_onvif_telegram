#include "telegram_parse.h"
#include <ArduinoJson.h>

std::vector<TelegramUpdate> parseTelegramUpdates(const String& jsonBody, String* error) {
  std::vector<TelegramUpdate> updates;

  JsonDocument doc;
  // .c_str(), not jsonBody directly - ArduinoJson's Arduino-String reader
  // specialization relies on platform detection that doesn't kick in
  // under ArduinoFake (env:native), falling back to a generic Stream-style
  // reader String doesn't implement. Passing a plain const char* sidesteps
  // that entirely and works identically on both the real firmware and
  // native tests.
  DeserializationError err = deserializeJson(doc, jsonBody.c_str());
  if (err) {
    if (error) *error = String("JSON parse failed: ") + err.c_str();
    return updates;
  }

  bool ok = doc["ok"] | false;
  if (!ok) {
    if (error) *error = String("Telegram API error: ") + (doc["description"] | "(no description)");
    return updates;
  }

  for (JsonObject update : doc["result"].as<JsonArray>()) {
    TelegramUpdate u;
    u.updateId = update["update_id"] | 0L;

    JsonObject message = update["message"];
    if (!message.isNull()) {
      JsonObject chat = message["chat"];
      if (!chat.isNull() && !chat["id"].isNull()) {
        u.chatId = chat["id"].as<long>();
        u.hasChatId = true;
      }
      // const char*, not .as<String>() - same reasoning as the .c_str()
      // above: ArduinoJson's String-aware conversion isn't available under
      // ArduinoFake, but every platform supports const char*, and String's
      // own constructor from one works everywhere regardless.
      const char* text = message["text"];
      if (text != nullptr) {
        u.text = String(text);
      }
    }
    // No "message" at all (e.g. edited_message, channel_post) - u keeps
    // hasChatId=false/text="", which the caller treats as "nothing to act
    // on", but updateId is still returned so its offset still advances.

    updates.push_back(u);
  }
  return updates;
}

std::vector<size_t> matchCamerasByPrefix(const CameraConfig cameras[], size_t numCameras, const String& needle) {
  String lowerNeedle = needle;
  lowerNeedle.toLowerCase();

  std::vector<size_t> matches;
  for (size_t i = 0; i < numCameras; i++) {
    if (!cameras[i].enabled) continue;
    String haystack = cameras[i].name;
    haystack.toLowerCase();
    if (haystack.startsWith(lowerNeedle)) matches.push_back(i);
  }
  return matches;
}

#pragma once
#include <Arduino.h>
#include <map>
#include <string>
#include <vector>
#include "user_form.h"

// Cameras offered as checkboxes; one name is HTML-hostile.
inline std::vector<CameraConfig> fixtureCameras() {
  std::vector<CameraConfig> cams(3);
  cams[0].name = "D01-Front";
  cams[1].name = "D02 <\"Back\">";
  cams[2].name = "d03-garage";
  return cams;
}

// The dashboard's blank Add form starts with All cameras ticked.
inline TelegramUser fixtureAddBlank() {
  TelegramUser u;
  u.allCameras = true;
  return u;
}

inline TelegramUser fixtureEditHostile() {
  TelegramUser u;
  u.name = "Ana <\"&'>";
  u.chatId = "-1001234567890\"";
  u.allCameras = false;
  u.cameraNames = {"d01-FRONT", "d03-garage"};  // matched case-insensitively
  u.systemMessages = false;
  u.canCommand = true;
  u.canSnap = true;
  u.canReset = true;
  u.canBackup = true;
  u.canRestore = true;
  u.maxCommandsPerMinute = 12;
  u.language = TelegramLang::Portuguese;
  return u;
}

class MapParams : public FormParams {
 public:
  std::map<std::string, std::string> values;
  bool has(const char* name) const override { return values.count(name) > 0; }
  String get(const char* name, const char* fallback) const override {
    auto it = values.find(name);
    return String(it == values.end() ? fallback : it->second.c_str());
  }
};

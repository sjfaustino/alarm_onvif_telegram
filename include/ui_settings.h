#pragma once
#include <Arduino.h>

// Dashboard-wide UI preference - persisted in NVS (Preferences, namespace
// "uisettings"), same tiny-settings-blob shape as sd_store.h's SdSettings.
// Server-authoritative (not a client-only toggle): renderShell()
// (webserver.cpp) reads this once per render and stamps the page's theme
// accordingly, so the preference survives across devices/browsers, not
// just one browser's local storage.
struct UiSettings {
  bool darkMode = false;
};
UiSettings loadUiSettings();
bool saveUiSettings(const UiSettings& settings);

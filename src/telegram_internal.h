#pragma once
// Shared between the telegram_*.cpp / snapshot_fetch.cpp files only - not
// part of telegram.h's public API.
#include <Arduino.h>
#include <utility>
#include <vector>
#include "camera.h"

// Serializes every TLS session to api.telegram.org project-wide, with a
// bounded wait (TELEGRAM_NET_MUTEX_TIMEOUT_MS). held()==false is a failed
// send, never an indefinite block. Defined in telegram_transport.cpp.
class TelegramNetLock {
 public:
  TelegramNetLock();
  ~TelegramNetLock();
  bool held() const { return held_; }
  TelegramNetLock(const TelegramNetLock&) = delete;
  TelegramNetLock& operator=(const TelegramNetLock&) = delete;

 private:
  bool held_;
};

// telegram_transport.cpp
bool sendTelegramMessageTo(const String& chatId, const String& text);
bool sendTelegramKeyboardTo(const String& chatId, const String& text,
                            const std::vector<std::pair<String, String>>& buttons,
                            size_t* outSkipped = nullptr);
bool answerTelegramCallback(const String& callbackQueryId, const String& text);
bool sendTelegramPhotoWithRetry(const uint8_t* jpg, size_t jpgLen, const String& caption,
                                const String& chatId);
bool sendTelegramDocumentBuffered(const String& content, const String& filename, const String& caption,
                                  const String& chatId);
String nowTimestampString();
bool localClockSynced();
int currentLocalMinuteOfDay();

// snapshot_fetch.cpp - caller must free() a non-null result.
uint8_t* fetchOneSnapshot(const CameraConfig& cfg, CameraState& st, size_t& outLen);

#pragma once
// Force-included into src/ (build_src_flags in platformio.ini), so every
// Serial.print there also lands in a RAM ring the /debug/log route can serve.
// There is no other way to reach the logs on a board with no cable attached.
#include <Arduino.h>

inline auto& logTeeRealSerial() { return Serial; }

class LogTee : public Print {
 public:
  void begin(unsigned long baud);
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* data, size_t len) override;
  using Print::write;
};
extern LogTee g_logTee;

#undef Serial
#define Serial g_logTee

// Copies up to maxLen log bytes from position `from` (see LogRing::read). Returns
// 0 if the ring doesn't exist or is busy.
size_t logTeeRead(uint32_t from, char* out, size_t maxLen, uint32_t* next, uint32_t* oldest);
// Position of the end of the log.
uint32_t logTeeEnd();

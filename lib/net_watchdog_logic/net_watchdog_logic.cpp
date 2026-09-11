#include "net_watchdog_logic.h"

bool isReservedOrUnsafePin(int pin) {
  // Already used by another peripheral in this project - config.h.
  static const int kReservedByThisProject[] = {10, 12, 13, 11, 8, 9}; // SD_CS/SCK/MISO/MOSI, RTC_SDA/SCL
  for (int reserved : kReservedByThisProject) {
    if (pin == reserved) return true;
  }

  // ESP32-S3 strapping pins - can affect boot mode if driven the wrong way
  // at reset.
  if (pin == 0 || pin == 3 || pin == 45 || pin == 46) return true;

  // Reserved for SPI flash (26-32) and, on this board's octal PSRAM
  // memory type (qio_opi - platformio.ini's BOARD_HAS_PSRAM/memory_type
  // comment), the extra PSRAM data/DQS lines (33-37) too - driving any of
  // these is a hard hang on boot, not just a wrong pin. Deliberately
  // conservative (blocking the whole 26-37 range rather than trying to
  // track exactly which sub-range a specific module variant uses) given
  // the consequence of getting this wrong on an unattended, remotely-
  // deployed board: stuck until someone physically re-flashes it.
  if (pin >= 26 && pin <= 37) return true;

  // UART0 (43/44 - this project's own Serial.begin(115200) logging) and
  // the USB-JTAG pins (19/20) - not a hard-hang risk the way the PSRAM
  // range is, but commandeering either would break the Serial log this
  // project relies on for diagnosing exactly this kind of problem, or the
  // board's USB console.
  if (pin == 19 || pin == 20 || pin == 43 || pin == 44) return true;

  // Negative or implausibly large - not a real GPIO number at all (a
  // hand-edited/imported NVS record isn't otherwise range-checked before
  // reaching here).
  if (pin < 0 || pin > 48) return true;

  return false;
}

bool outageThresholdReached(unsigned long firstFailureMs, unsigned long nowMs, uint32_t thresholdMs) {
  return (nowMs - firstFailureMs) >= thresholdMs;
}

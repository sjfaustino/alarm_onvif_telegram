#include "net_watchdog_logic.h"

bool isReservedOrUnsafePin(int pin) {
  static const int kReservedByThisProject[] = {10, 12, 13, 11, 8, 9}; // SD_CS/SCK/MISO/MOSI, RTC_SDA/SCL
  for (int reserved : kReservedByThisProject) {
    if (pin == reserved) return true;
  }

  // Strapping pins: can change boot mode.
  if (pin == 0 || pin == 3 || pin == 45 || pin == 46) return true;

  // SPI flash (26-32) and octal PSRAM (33-37); driving them hangs the board.
  // The whole range is blocked - an unattended board would need re-flashing.
  if (pin >= 26 && pin <= 37) return true;

  // USB-JTAG (19/20) and UART0 (43/44, the Serial log).
  if (pin == 19 || pin == 20 || pin == 43 || pin == 44) return true;

  // Not a GPIO at all.
  if (pin < 0 || pin > 48) return true;

  return false;
}

bool outageThresholdReached(unsigned long firstFailureMs, unsigned long nowMs, uint32_t thresholdMs) {
  return (nowMs - firstFailureMs) >= thresholdMs;
}

bool watchdogPinsConflict(bool enabledA, int pinA, bool enabledB, int pinB) {
  return enabledA && enabledB && pinA == pinB;
}

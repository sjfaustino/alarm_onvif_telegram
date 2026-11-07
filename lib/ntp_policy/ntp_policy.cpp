#include "ntp_policy.h"

bool shouldRestartSntp(bool started, bool syncedSinceStart, unsigned long msSinceSync, unsigned long msSinceAttempt,
                       unsigned long syncIntervalMs, unsigned long retryMs) {
  if (msSinceAttempt < retryMs) return false;
  if (!started || !syncedSinceStart) return true;
  return msSinceSync / 3 > syncIntervalMs; // divide, so a 30-day interval can't overflow
}

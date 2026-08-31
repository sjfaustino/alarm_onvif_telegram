#include "subscription_health.h"

SubscriptionHealthResult evaluateSubscriptionHealth(bool isOffline, unsigned long msSinceLastSubscribed,
                                                      unsigned long thresholdMs, bool alreadyAlerted) {
  if (isOffline) return {false, false}; // already covered by the OFFLINE alert - re-arm for next time

  if (msSinceLastSubscribed < thresholdMs) return {false, false}; // subscribed recently enough - re-arm

  if (alreadyAlerted) return {true, false}; // already alerted for this stretch

  return {true, true}; // first call past the threshold - fire the alert
}

#pragma once

// Pure alert-once/re-arm logic behind checkSubscriptionHealth, tested
// natively: catches a camera that answers (even with SOAP faults) but never
// holds a subscription, which the OFFLINE alert can't see.
// msSinceLastSubscribed is computed by the caller.
struct SubscriptionHealthResult {
  bool alerted;     // new value for CameraState::subscriptionLostAlerted
  bool shouldAlert; // true only on the one call that should actually send the Telegram alert
};

// While isOffline (already alerted as OFFLINE), never alerts, and re-arms.
SubscriptionHealthResult evaluateSubscriptionHealth(bool isOffline, unsigned long msSinceLastSubscribed,
                                                      unsigned long thresholdMs, bool alreadyAlerted);
